use std::sync::Arc;

use bragi::Message;
use hel::{Accept, Handle, PushDescriptor, ReceiveInline, SendBuffer, submit_async};

use crate::hw::bindings::Errors;

use super::bindings;
use super::pci::IoType;

fn encode_hw_error(err: &super::Error) -> Errors {
    match err {
        super::Error::OutOfBounds => Errors::OutOfBounds,
        super::Error::IllegalArguments => Errors::IllegalArguments,
        super::Error::ResourceExhaustion => Errors::ResourceExhaustion,
        super::Error::PropertyNotFound => Errors::PropertyNotFound,
        _ => Errors::DeviceError,
    }
}

#[derive(Debug, Clone, Copy, Default)]
pub struct BarDescriptor {
    pub io_type: IoType,
    pub host_type: IoType,
    pub address: u64,
    pub length: u64,
    pub offset: u32,
}

#[derive(Debug, Clone, Copy, Default)]
pub struct CapDescriptor {
    pub type_: u32,
    pub offset: u64,
    pub length: u64,
}

pub trait PciDevice {
    fn bars(&self) -> [BarDescriptor; 6];
    fn capabilities(&self) -> Vec<CapDescriptor>;
    fn expansion_rom(&self) -> (u64, u64) {
        (0, 0)
    }
    fn num_msis(&self) -> u32 {
        0
    }
    fn msi_x(&self) -> bool {
        false
    }

    fn config_read(&self, offset: u32, size: u32) -> Option<u32>;
    fn config_write(&self, offset: u32, size: u32, word: u32) -> bool;
    fn capability_read(&self, index: i32, offset: u32, size: u32) -> Option<u32>;

    fn access_bar(&self, index: usize) -> hel::Result<Handle>;
    fn access_irq(&self, index: u64) -> hel::Result<Option<Handle>>;
    fn access_expansion_rom(&self) -> hel::Result<Option<Handle>> {
        Ok(None)
    }
    fn access_vbt(&self) -> hel::Result<Option<(u32, Handle)>> {
        Ok(None)
    }
    fn install_msi(&self, index: u32) -> hel::Result<Option<Handle>> {
        let _ = index;
        Ok(None)
    }

    fn enable_busmaster(&self);
    fn enable_irq(&self);
    fn enable_msi(&self) -> bool {
        false
    }
    /// Attaches the device to its IOMMU domain, or to the passthrough domain.
    async fn enable_dma(&self, _passthrough: bool) -> bool {
        false
    }
    fn get_dma_space(&self) -> hel::Result<(bool, Handle)> {
        Ok((false, hel::create_dma_space(None, &[])?))
    }
    /// Whether DMA by the device snoops the CPU caches. Unless the server knows, drivers have
    /// to assume that it does not.
    fn dma_coherent(&self) -> bool {
        false
    }
    fn claim_device(&self) {}
}

fn encode_io_type(t: IoType) -> bindings::IoType {
    match t {
        IoType::None => bindings::IoType::NoBar,
        IoType::Port => bindings::IoType::Port,
        IoType::Memory => bindings::IoType::Memory,
    }
}

fn build_pci_info<D: PciDevice>(device: &D) -> bindings::SvrResponse {
    let mut resp = bindings::SvrResponse::default();
    resp.set_error(Errors::Success);

    if device.num_msis() > 0 {
        resp.set_num_msis(device.num_msis());
        resp.set_msi_x(device.msi_x() as u8);
    }

    let caps = device
        .capabilities()
        .into_iter()
        .map(|cap| {
            let mut msg = bindings::PciCapability::default();
            msg.set_type(cap.type_);
            msg.set_offset(cap.offset);
            msg.set_length(cap.length);
            msg
        })
        .collect();
    resp.set_capabilities(caps);

    let bars = device
        .bars()
        .iter()
        .map(|bar| {
            let mut msg = bindings::PciBar::default();
            msg.set_io_type(encode_io_type(bar.io_type));
            msg.set_host_type(encode_io_type(bar.host_type));
            if bar.host_type != IoType::None {
                msg.set_address(bar.address);
                msg.set_length(bar.length);
                msg.set_offset(bar.offset);
            }
            msg
        })
        .collect();
    resp.set_bars(bars);

    let (rom_addr, rom_len) = device.expansion_rom();
    let mut rom = bindings::PciExpansionRom::default();
    rom.set_address(rom_addr);
    rom.set_length(rom_len);
    resp.set_expansion_rom(rom);

    resp
}

async fn send_response<M: Message>(lane: &Handle, resp: &M) -> hel::Result<()> {
    let (head, tail) = bragi::head_tail_to_bytes(resp).expect("failed to encode hw response");
    let (head, tail) = submit_async(lane, (SendBuffer::new(&head), SendBuffer::new(&tail))).await?;
    head?;
    tail?;
    Ok(())
}

async fn send_response_with_push<M: Message>(
    lane: &Handle,
    resp: &M,
    push: &Handle,
    rights: u32,
) -> hel::Result<()> {
    let (head, tail) = bragi::head_tail_to_bytes(resp).expect("failed to encode hw response");
    let (head, tail, push) = submit_async(
        lane,
        (
            SendBuffer::new(&head),
            SendBuffer::new(&tail),
            PushDescriptor::new(push, rights),
        ),
    )
    .await?;
    (head?, tail?, push?);
    Ok(())
}

async fn send_dma_space(
    lane: &Handle,
    iommu_active: bool,
    dma_coherent: bool,
    space: &Handle,
) -> hel::Result<()> {
    let mut resp = bindings::GetDmaSpaceResponse::default();
    resp.set_iommu_active(iommu_active as i8);
    resp.set_dma_coherent(dma_coherent as i8);
    let head = bragi::head_to_bytes(&resp).expect("failed to encode hw response");
    let (head, push) = submit_async(
        lane,
        (
            SendBuffer::new(&head),
            PushDescriptor::new(space, hel_sys::kHelRightGrant | hel_sys::kHelRightProvision),
        ),
    )
    .await?;
    (head?, push?);
    Ok(())
}

fn error_response(error: Errors) -> bindings::SvrResponse {
    let mut resp = bindings::SvrResponse::default();
    resp.set_error(error);
    resp
}

async fn handle_one<D: PciDevice>(lane: &Handle, request: &[u8], device: &D) -> hel::Result<()> {
    let preamble = match bragi::preamble_from_bytes(request) {
        Ok(p) => p,
        Err(_) => return Ok(()),
    };
    let id = preamble.id();

    match id {
        bindings::GetPciInfoRequest::MESSAGE_ID => {
            let resp = build_pci_info(device);
            send_response(lane, &resp).await?;
        }
        bindings::AccessBarRequest::MESSAGE_ID => {
            let req: bindings::AccessBarRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            match device.access_bar(req.index() as usize) {
                Ok(handle) => {
                    send_response_with_push(
                        lane,
                        &error_response(Errors::Success),
                        &handle,
                        hel_sys::kHelRightRead
                            | hel_sys::kHelRightWrite
                            | hel_sys::kHelRightAssign
                            | hel_sys::kHelRightProvision
                            | hel_sys::kHelRightPin,
                    )
                    .await?
                }
                Err(_) => send_response(lane, &error_response(Errors::OutOfBounds)).await?,
            }
        }
        bindings::AccessIrqRequest::MESSAGE_ID => {
            let req: bindings::AccessIrqRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            match device.access_irq(req.index()) {
                Ok(Some(handle)) => {
                    send_response_with_push(
                        lane,
                        &error_response(Errors::Success),
                        &handle,
                        hel_sys::kHelRightWait | hel_sys::kHelRightSignal,
                    )
                    .await?
                }
                _ => send_response(lane, &error_response(Errors::IllegalArguments)).await?,
            }
        }
        bindings::AccessExpansionRomRequest::MESSAGE_ID => match device.access_expansion_rom() {
            Ok(Some(handle)) => {
                send_response_with_push(
                    lane,
                    &error_response(Errors::Success),
                    &handle,
                    hel_sys::kHelRightRead
                        | hel_sys::kHelRightAssign
                        | hel_sys::kHelRightProvision
                        | hel_sys::kHelRightPin,
                )
                .await?
            }
            _ => send_response(lane, &error_response(Errors::DeviceError)).await?,
        },
        bindings::GetVbtRequest::MESSAGE_ID => match device.access_vbt() {
            Ok(Some((size, handle))) => {
                let mut resp = bindings::SvrResponse::default();
                resp.set_error(Errors::Success);
                resp.set_vbt_size(size);
                send_response_with_push(
                    lane,
                    &resp,
                    &handle,
                    hel_sys::kHelRightRead
                        | hel_sys::kHelRightAssign
                        | hel_sys::kHelRightProvision
                        | hel_sys::kHelRightPin,
                )
                .await?
            }
            _ => send_response(lane, &error_response(Errors::IllegalArguments)).await?,
        },
        bindings::LoadPciSpaceRequest::MESSAGE_ID => {
            let req: bindings::LoadPciSpaceRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            let mut resp = bindings::SvrResponse::default();
            match device.config_read(req.offset(), req.size()) {
                Some(word) => {
                    resp.set_error(Errors::Success);
                    resp.set_word(word);
                }
                None => resp.set_error(Errors::IllegalArguments),
            }
            send_response(lane, &resp).await?;
        }
        bindings::StorePciSpaceRequest::MESSAGE_ID => {
            let req: bindings::StorePciSpaceRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            let error = if device.config_write(req.offset(), req.size(), req.word()) {
                Errors::Success
            } else {
                Errors::IllegalArguments
            };
            send_response(lane, &error_response(error)).await?;
        }
        bindings::LoadPciCapabilityRequest::MESSAGE_ID => {
            let req: bindings::LoadPciCapabilityRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            let mut resp = bindings::SvrResponse::default();
            match device.capability_read(req.index(), req.offset(), req.size()) {
                Some(word) => {
                    resp.set_error(Errors::Success);
                    resp.set_word(word);
                }
                None => resp.set_error(Errors::IllegalArguments),
            }
            send_response(lane, &resp).await?;
        }
        bindings::EnableBusmasterRequest::MESSAGE_ID => {
            device.enable_busmaster();
            send_response(lane, &error_response(Errors::Success)).await?;
        }
        bindings::EnableBusIrqRequest::MESSAGE_ID => {
            device.enable_irq();
            send_response(lane, &error_response(Errors::Success)).await?;
        }
        bindings::InstallMsiRequest::MESSAGE_ID => {
            let req: bindings::InstallMsiRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            match device.install_msi(req.index()) {
                Ok(Some(handle)) => {
                    send_response_with_push(
                        lane,
                        &error_response(Errors::Success),
                        &handle,
                        hel_sys::kHelRightWait | hel_sys::kHelRightSignal,
                    )
                    .await?
                }
                Ok(None) => send_response(lane, &error_response(Errors::IllegalArguments)).await?,
                Err(_) => send_response(lane, &error_response(Errors::ResourceExhaustion)).await?,
            }
        }
        bindings::EnableMsiRequest::MESSAGE_ID => {
            if device.enable_msi() {
                send_response(lane, &error_response(Errors::Success)).await?;
            } else {
                send_response(lane, &error_response(Errors::IllegalArguments)).await?;
            }
        }
        bindings::EnableDmaRequest::MESSAGE_ID => {
            let req: bindings::EnableDmaRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            let error = if device.enable_dma(req.passthrough() != 0).await {
                Errors::Success
            } else {
                Errors::DeviceError
            };
            send_response(lane, &error_response(error)).await?;
        }
        bindings::GetDmaSpaceRequest::MESSAGE_ID => {
            let (iommu_active, space) = device.get_dma_space()?;
            send_dma_space(lane, iommu_active, device.dma_coherent(), &space).await?;
        }
        bindings::ClaimDeviceRequest::MESSAGE_ID => {
            device.claim_device();
            send_response(lane, &error_response(Errors::Success)).await?;
        }
        _ => send_response(lane, &error_response(Errors::DeviceError)).await?,
    }

    Ok(())
}

async fn serve_requests(lane: Handle, handler: impl AsyncFn(&Handle, &[u8]) -> hel::Result<()>) {
    loop {
        let (conversation, request) = match submit_async(&lane, Accept::new(ReceiveInline)).await {
            Ok((conversation, request)) => (conversation, request),
            Err(_) => break,
        };

        let conversation = match conversation {
            Ok(Some(conversation)) => conversation,
            _ => break,
        };

        let request = match request {
            Ok(request) => request,
            Err(_) => continue,
        };

        if handler(&conversation, &request).await.is_err() {
            continue;
        }
    }
}

pub async fn serve_pci_device<D: PciDevice + 'static>(lane: Handle, device: Arc<D>) {
    serve_requests(lane, async |conversation: &Handle, request: &[u8]| {
        handle_one(conversation, request, device.as_ref()).await
    })
    .await
}

/// A memory resource of an ACPI object's _CRS.
#[derive(Debug, Clone, Copy, Default)]
pub struct AcpiMemoryRange {
    pub address: u64,
    pub length: u64,
    /// Offset of the range into the memory view that access_memory() returns.
    pub offset: u32,
}

/// The IO ports, IRQs and memory ranges that an ACPI object's _CRS describes.
#[derive(Debug, Default)]
pub struct AcpiResources {
    pub io_ports: Vec<u16>,
    pub fixed_io_ports: Vec<u16>,
    pub irqs: Vec<u32>,
    pub memory_ranges: Vec<AcpiMemoryRange>,
}

pub trait AcpiObject {
    /// Returns the resources of the object's _CRS, or None if evaluation fails.
    fn resources(&self) -> Option<AcpiResources>;
    /// Returns an IO-space handle covering the ports of the index-th port resource of _CRS.
    fn access_ports(&self, index: usize) -> super::Result<Handle>;
    /// Returns the IRQ object for the index-th interrupt of _CRS.
    fn access_irq(&self, index: usize) -> super::Result<&Handle>;
    /// Returns a memory view of the index-th memory resource of _CRS.
    fn access_memory(&self, _index: usize) -> super::Result<Handle> {
        Err(super::Error::OutOfBounds)
    }
    fn get_dma_space(&self) -> hel::Result<(bool, Handle)> {
        Ok((false, hel::create_dma_space(None, &[])?))
    }
    /// Whether DMA by the device snoops the CPU caches. Unless the server knows, drivers have
    /// to assume that it does not.
    fn dma_coherent(&self) -> bool {
        false
    }
}

async fn handle_one_acpi<D: AcpiObject>(
    lane: &Handle,
    request: &[u8],
    object: &D,
) -> hel::Result<()> {
    let preamble = match bragi::preamble_from_bytes(request) {
        Ok(p) => p,
        Err(_) => return Ok(()),
    };

    match preamble.id() {
        bindings::AcpiGetResourcesRequest::MESSAGE_ID => {
            let mut resp = bindings::AcpiGetResourcesReply::default();
            match object.resources() {
                Some(resources) => {
                    resp.set_error(Errors::Success);
                    resp.set_io_ports(resources.io_ports);
                    resp.set_fixed_io_ports(resources.fixed_io_ports);
                    resp.set_irqs(resources.irqs);
                    let ranges = resources
                        .memory_ranges
                        .iter()
                        .map(|range| {
                            let mut msg = bindings::AcpiMemoryRange::default();
                            msg.set_address(range.address);
                            msg.set_length(range.length);
                            msg.set_offset(range.offset);
                            msg
                        })
                        .collect();
                    resp.set_memory_ranges(ranges);
                }
                None => resp.set_error(Errors::DeviceError),
            }
            send_response(lane, &resp).await?;
        }
        bindings::AccessBarRequest::MESSAGE_ID => {
            let req: bindings::AccessBarRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            let index = usize::try_from(req.index()).map_err(|_| hel::Error::IllegalArgs)?;
            match object.access_ports(index) {
                Ok(handle) => {
                    send_response_with_push(
                        lane,
                        &error_response(Errors::Success),
                        &handle,
                        hel_sys::kHelRightRead
                            | hel_sys::kHelRightWrite
                            | hel_sys::kHelRightAssign
                            | hel_sys::kHelRightProvision
                            | hel_sys::kHelRightPin,
                    )
                    .await?
                }
                Err(err) => send_response(lane, &error_response(encode_hw_error(&err))).await?,
            }
        }
        bindings::AccessIrqRequest::MESSAGE_ID => {
            let req: bindings::AccessIrqRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            let index = usize::try_from(req.index()).map_err(|_| hel::Error::IllegalArgs)?;
            match object.access_irq(index) {
                Ok(handle) => {
                    send_response_with_push(
                        lane,
                        &error_response(Errors::Success),
                        handle,
                        hel_sys::kHelRightWait | hel_sys::kHelRightSignal,
                    )
                    .await?
                }
                Err(err) => send_response(lane, &error_response(encode_hw_error(&err))).await?,
            }
        }
        bindings::AcpiAccessMemoryRequest::MESSAGE_ID => {
            let req: bindings::AcpiAccessMemoryRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            match object.access_memory(req.index() as usize) {
                Ok(handle) => {
                    send_response_with_push(
                        lane,
                        &error_response(Errors::Success),
                        &handle,
                        hel_sys::kHelRightRead
                            | hel_sys::kHelRightWrite
                            | hel_sys::kHelRightAssign
                            | hel_sys::kHelRightProvision
                            | hel_sys::kHelRightPin,
                    )
                    .await?
                }
                Err(err) => send_response(lane, &error_response(encode_hw_error(&err))).await?,
            }
        }
        bindings::GetDmaSpaceRequest::MESSAGE_ID => {
            let (iommu_active, space) = object.get_dma_space()?;
            send_dma_space(lane, iommu_active, object.dma_coherent(), &space).await?;
        }
        _ => send_response(lane, &error_response(Errors::DeviceError)).await?,
    }

    Ok(())
}

pub async fn serve_acpi_object<D: AcpiObject + 'static>(lane: Handle, object: Arc<D>) {
    serve_requests(lane, async |conversation: &Handle, request: &[u8]| {
        handle_one_acpi(conversation, request, object.as_ref()).await
    })
    .await
}

/// The state of a battery.
/// The units are the ones of the hw protocol: µV, µA, µAh and µWh.
#[derive(Debug, Clone, Copy, Default)]
pub struct BatteryState {
    pub charging: bool,
    pub current_now: Option<u64>,
    pub power_now: Option<u64>,
    pub energy_now: Option<u64>,
    pub energy_full: Option<u64>,
    pub energy_full_design: Option<u64>,
    pub voltage_now: Option<u64>,
    pub voltage_min_design: Option<u64>,
}

pub trait Battery {
    fn state(&self, block_until_ready: bool) -> impl std::future::Future<Output = BatteryState>;
}

async fn handle_one_battery<B: Battery>(
    lane: &Handle,
    request: &[u8],
    battery: &B,
) -> hel::Result<()> {
    let preamble = match bragi::preamble_from_bytes(request) {
        Ok(p) => p,
        Err(_) => return Ok(()),
    };

    match preamble.id() {
        bindings::BatteryStateRequest::MESSAGE_ID => {
            let req: bindings::BatteryStateRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            let state = battery.state(req.block_until_ready() != 0).await;

            let mut resp = bindings::BatteryStateReply::default();
            resp.set_error(Errors::Success);
            resp.set_charging(state.charging.into());
            // Fields that the battery does not report are left out of the reply.
            if let Some(current_now) = state.current_now {
                resp.set_current_now(current_now);
            }
            if let Some(power_now) = state.power_now {
                resp.set_power_now(power_now);
            }
            if let Some(energy_now) = state.energy_now {
                resp.set_energy_now(energy_now);
            }
            if let Some(energy_full) = state.energy_full {
                resp.set_energy_full(energy_full);
            }
            if let Some(energy_full_design) = state.energy_full_design {
                resp.set_energy_full_design(energy_full_design);
            }
            if let Some(voltage_now) = state.voltage_now {
                resp.set_voltage_now(voltage_now);
            }
            if let Some(voltage_min_design) = state.voltage_min_design {
                resp.set_voltage_min_design(voltage_min_design);
            }
            send_response(lane, &resp).await?;
        }
        _ => send_response(lane, &error_response(Errors::DeviceError)).await?,
    }

    Ok(())
}

pub async fn serve_battery<B: Battery + 'static>(lane: Handle, battery: Arc<B>) {
    serve_requests(lane, async |conversation: &Handle, request: &[u8]| {
        handle_one_battery(conversation, request, battery.as_ref()).await
    })
    .await
}

/// A register range of a device tree node's reg property.
#[derive(Debug, Clone, Copy, Default)]
pub struct DtRegisterDescriptor {
    pub address: u64,
    pub length: u64,
    pub offset: u32,
}

pub trait DtNode {
    fn regs(&self) -> Vec<DtRegisterDescriptor>;
    fn num_irqs(&self) -> u32;
    fn path(&self) -> String;
    fn property(&self, name: &str) -> Option<Vec<u8>>;
    fn properties(&self) -> Vec<(String, Vec<u8>)>;

    /// Returns a memory view of the index-th register range.
    fn access_register(&self, index: usize) -> super::Result<Handle>;
    /// Returns the IRQ object for the index-th interrupt of the node.
    fn install_irq(&self, index: usize) -> super::Result<&Handle>;
    /// Configures all interrupts of the node.
    fn enable_irqs(&self);
}

async fn handle_one_dt<D: DtNode>(lane: &Handle, request: &[u8], node: &D) -> hel::Result<()> {
    let preamble = match bragi::preamble_from_bytes(request) {
        Ok(p) => p,
        Err(_) => return Ok(()),
    };

    match preamble.id() {
        bindings::GetDtInfoRequest::MESSAGE_ID => {
            let mut resp = bindings::SvrResponse::default();
            resp.set_error(Errors::Success);
            resp.set_num_dt_irqs(node.num_irqs());
            let regs = node
                .regs()
                .iter()
                .map(|reg| {
                    let mut msg = bindings::DtRegister::default();
                    msg.set_address(reg.address);
                    msg.set_length(reg.length);
                    msg.set_offset(reg.offset);
                    msg
                })
                .collect();
            resp.set_dt_regs(regs);
            send_response(lane, &resp).await?;
        }
        bindings::AccessDtRegisterRequest::MESSAGE_ID => {
            let req: bindings::AccessDtRegisterRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            match node.access_register(req.index() as usize) {
                Ok(handle) => {
                    send_response_with_push(
                        lane,
                        &error_response(Errors::Success),
                        &handle,
                        hel_sys::kHelRightRead
                            | hel_sys::kHelRightWrite
                            | hel_sys::kHelRightAssign
                            | hel_sys::kHelRightProvision
                            | hel_sys::kHelRightPin,
                    )
                    .await?
                }
                Err(err) => send_response(lane, &error_response(encode_hw_error(&err))).await?,
            }
        }
        bindings::InstallDtIrqRequest::MESSAGE_ID => {
            let req: bindings::InstallDtIrqRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            match node.install_irq(req.index() as usize) {
                Ok(handle) => {
                    send_response_with_push(
                        lane,
                        &error_response(Errors::Success),
                        handle,
                        hel_sys::kHelRightWait | hel_sys::kHelRightSignal,
                    )
                    .await?
                }
                Err(err) => send_response(lane, &error_response(encode_hw_error(&err))).await?,
            }
        }
        bindings::EnableBusIrqRequest::MESSAGE_ID => {
            node.enable_irqs();
            send_response(lane, &error_response(Errors::Success)).await?;
        }
        bindings::GetDtPropertyRequest::MESSAGE_ID => {
            let req: bindings::GetDtPropertyRequest =
                bragi::head_from_bytes(request).map_err(|_| hel::Error::IllegalArgs)?;
            let mut resp = bindings::GetDtPropertyResponse::default();
            match node.property(req.name()) {
                Some(data) => {
                    resp.set_error(Errors::Success);
                    resp.set_data(data);
                }
                None => resp.set_error(Errors::PropertyNotFound),
            }
            send_response(lane, &resp).await?;
        }
        bindings::GetDtPropertiesRequest::MESSAGE_ID => {
            let mut resp = bindings::GetDtPropertiesResponse::default();
            resp.set_error(Errors::Success);
            let properties = node
                .properties()
                .into_iter()
                .map(|(name, data)| {
                    let mut msg = bindings::DtProperty::default();
                    msg.set_name(name);
                    msg.set_data(data);
                    msg
                })
                .collect();
            resp.set_properties(properties);
            send_response(lane, &resp).await?;
        }
        bindings::GetDtPathRequest::MESSAGE_ID => {
            let mut resp = bindings::GetDtPathResponse::default();
            resp.set_error(Errors::Success);
            resp.set_path(node.path());
            send_response(lane, &resp).await?;
        }
        _ => send_response(lane, &error_response(Errors::DeviceError)).await?,
    }

    Ok(())
}

pub async fn serve_dt_node<D: DtNode + 'static>(lane: Handle, node: Arc<D>) {
    serve_requests(lane, async |conversation: &Handle, request: &[u8]| {
        handle_one_dt(conversation, request, node.as_ref()).await
    })
    .await
}
