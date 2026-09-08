use bragi::Message;
use hel::Handle;

use super::{Error, Result, bindings, pci::PciInfo};

pub struct Device {
    handle: Handle,
}

/// The state that a battery reports; absent fields are not supported by the battery.
#[derive(Debug, Clone, Default)]
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

/// A device tree property: its name and raw value.
#[derive(Debug, Clone)]
pub struct DtProperty {
    pub name: String,
    pub data: Vec<u8>,
}

impl Device {
    pub fn new(handle: Handle) -> Self {
        Self { handle }
    }

    /// Sends a head-only request and receives a head+tail response on the offered lane.
    async fn request_tailed<Req: Message, Resp: Message + Default>(
        &self,
        req: &Req,
    ) -> Result<Resp> {
        let head = bragi::head_to_bytes(req)?;
        let (offer, (_send_head, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let preamble = bragi::preamble_from_bytes(&recv_data)?;
        let mut tail_buffer = vec![0; preamble.tail_size() as usize];

        hel::submit_async(
            &conversation_lane,
            hel::ReceiveBuffer::new(&mut tail_buffer),
        )
        .await??; // Handle both the submit_async and receive errors

        Ok(bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?)
    }

    /// Sends a head-only request whose response is a head followed by a raw
    /// data buffer of the size that the response head announces.
    async fn request_sized_blob<Req: Message, Resp: Message + Default>(
        &self,
        req: &Req,
        size_of: impl FnOnce(&Resp) -> Result<usize>,
    ) -> Result<Vec<u8>> {
        let head = bragi::head_to_bytes(req)?;
        let (offer, (_send_head, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let response: Resp = bragi::head_from_bytes(&recv_data)?;
        let mut data = vec![0; size_of(&response)?];

        hel::submit_async(&conversation_lane, hel::ReceiveBuffer::new(&mut data)).await??;
        Ok(data)
    }

    pub async fn get_pci_info(&self) -> Result<PciInfo> {
        let response: bindings::SvrResponse = self
            .request_tailed(&bindings::GetPciInfoRequest::new())
            .await?;

        if response.error() == bindings::Errors::Success {
            Ok(PciInfo::from(&response))
        } else {
            Err(Error::from(response.error()))
        }
    }

    /// Reads `size` bytes (1, 2 or 4) of PCI configuration space at `offset`.
    pub async fn load_pci_space(&self, offset: u32, size: u32) -> Result<u32> {
        let response: bindings::SvrResponse = self
            .request_tailed(&bindings::LoadPciSpaceRequest::new(offset, size))
            .await?;

        if response.error() == bindings::Errors::Success {
            Ok(response.word().unwrap_or(0))
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn access_bar(&self, bar: usize) -> Result<Handle> {
        let head = bragi::head_to_bytes(&bindings::AccessBarRequest::new(bar as i32))?;
        let (offer, (_send, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let preamble = bragi::preamble_from_bytes(&recv_data)?;
        let mut tail_buffer = vec![0; preamble.tail_size() as usize];
        let (_recv, pull) = hel::submit_async(
            &conversation_lane,
            (
                hel::ReceiveBuffer::new(&mut tail_buffer),
                hel::PullDescriptor::new(
                    hel_sys::kHelRightRead | hel_sys::kHelRightWrite | hel_sys::kHelRightAssign,
                ),
            ),
        )
        .await?;

        let response: bindings::SvrResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Errors::Success {
            Ok(pull?.expect("No descriptor pushed"))
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn access_irq(&self, index: usize) -> Result<Handle> {
        let head = bragi::head_to_bytes(&bindings::AccessIrqRequest::new(index as u64))?;
        let (offer, (_send_head, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let preamble = bragi::preamble_from_bytes(&recv_data)?;
        let mut tail_buffer = vec![0; preamble.tail_size() as usize];
        let (_recv, pull) = hel::submit_async(
            &conversation_lane,
            (
                hel::ReceiveBuffer::new(&mut tail_buffer),
                hel::PullDescriptor::new(hel_sys::kHelRightWait | hel_sys::kHelRightSignal),
            ),
        )
        .await?;

        let response: bindings::SvrResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Errors::Success {
            Ok(pull?.expect("No descriptor pushed"))
        } else {
            Err(Error::from(response.error()))
        }
    }

    /// Reads the battery state; with `block` the driver only answers once the
    /// state has changed since the previous call.
    pub async fn get_battery_state(&self, block: bool) -> Result<BatteryState> {
        let response: bindings::BatteryStateReply = self
            .request_tailed(&bindings::BatteryStateRequest::new(0, block as u32))
            .await?;

        if response.error() != bindings::Errors::Success {
            return Err(Error::from(response.error()));
        }
        Ok(BatteryState {
            charging: response.charging().unwrap_or(0) != 0,
            current_now: response.current_now(),
            power_now: response.power_now(),
            energy_now: response.energy_now(),
            energy_full: response.energy_full(),
            energy_full_design: response.energy_full_design(),
            voltage_now: response.voltage_now(),
            voltage_min_design: response.voltage_min_design(),
        })
    }

    pub async fn get_smbios_header(&self) -> Result<Vec<u8>> {
        self.request_sized_blob(
            &bindings::GetSmbiosHeaderRequest::new(),
            |resp: &bindings::GetSmbiosHeaderReply| {
                if resp.error() != bindings::Errors::Success {
                    return Err(Error::from(resp.error()));
                }
                Ok(resp.size() as usize)
            },
        )
        .await
    }

    pub async fn get_smbios_table(&self) -> Result<Vec<u8>> {
        self.request_sized_blob(
            &bindings::GetSmbiosTableRequest::new(),
            |resp: &bindings::GetSmbiosTableReply| {
                if resp.error() != bindings::Errors::Success {
                    return Err(Error::from(resp.error()));
                }
                Ok(resp.size() as usize)
            },
        )
        .await
    }

    /// Returns the full path of a device tree node, e.g. `/soc/serial@9000000`.
    pub async fn get_dt_path(&self) -> Result<String> {
        let response: bindings::GetDtPathResponse = self
            .request_tailed(&bindings::GetDtPathRequest::new())
            .await?;

        if response.error() == bindings::Errors::Success {
            Ok(response.path().to_string())
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn get_dt_properties(&self) -> Result<Vec<DtProperty>> {
        let response: bindings::GetDtPropertiesResponse = self
            .request_tailed(&bindings::GetDtPropertiesRequest::new())
            .await?;

        if response.error() != bindings::Errors::Success {
            return Err(Error::from(response.error()));
        }
        Ok(response
            .properties()
            .iter()
            .map(|property| DtProperty {
                name: property.name().to_string(),
                data: property.data().to_vec(),
            })
            .collect())
    }

    pub async fn enable_busmaster(&self) -> Result<()> {
        self.simple_request(&bindings::EnableBusmasterRequest::new())
            .await
    }

    pub async fn enable_bus_irq(&self) -> Result<()> {
        self.simple_request(&bindings::EnableBusIrqRequest::new())
            .await
    }

    pub async fn enable_msi(&self) -> Result<()> {
        self.simple_request(&bindings::EnableMsiRequest::new()).await
    }

    pub async fn install_msi(&self, index: usize) -> Result<Handle> {
        let head = bragi::head_to_bytes(&bindings::InstallMsiRequest::new(index as u32))?;
        let (offer, (_send_head, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let preamble = bragi::preamble_from_bytes(&recv_data)?;
        let mut tail_buffer = vec![0; preamble.tail_size() as usize];
        let (_recv, pull) = hel::submit_async(
            &conversation_lane,
            (
                hel::ReceiveBuffer::new(&mut tail_buffer),
                hel::PullDescriptor::new(hel_sys::kHelRightWait | hel_sys::kHelRightSignal),
            ),
        )
        .await?;

        let response: bindings::SvrResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Errors::Success {
            Ok(pull?.expect("No descriptor pushed"))
        } else {
            Err(Error::from(response.error()))
        }
    }

    // Sends a request that is answered by a plain SvrResponse without payload.
    async fn simple_request<M: Message>(&self, request: &M) -> Result<()> {
        let response: bindings::SvrResponse = self.request_tailed(request).await?;

        if response.error() == bindings::Errors::Success {
            Ok(())
        } else {
            Err(Error::from(response.error()))
        }
    }
}
