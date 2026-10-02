use std::collections::{BTreeMap, HashMap};
use std::ffi::CStr;
use std::sync::{Arc, Mutex};

use anyhow::Result;
use managarm::hw::Error as HwError;
use managarm::hw::server::{AcpiMemoryRange, AcpiObject, AcpiResources, serve_acpi_object};
use managarm::mbus::{EntityManager, create_entity};
use managarm::svrctl::hardware_access_handle;

use crate::acpi::{PAGE_MASK, PAGE_SIZE, eval_cca};
use crate::entity::{serve_entity_lanes, string};
use crate::leak;
use crate::uacpi::namespace::{self, IterationDecision, NamespaceNode};
use crate::uacpi::resources::{Memory, Resource};

const EXPECT_LOCK: &str = "sif: ACPI IRQ object mutex was poisoned";

pub struct NodeObject {
    node: NamespaceNode,
    irq_objects: Mutex<BTreeMap<usize, &'static hel::Handle>>,
}

impl NodeObject {
    pub fn new(node: NamespaceNode) -> NodeObject {
        NodeObject {
            node,
            irq_objects: Mutex::new(BTreeMap::new()),
        }
    }
}

fn port_list(resource: &Resource) -> Option<Vec<u16>> {
    match resource {
        Resource::Io(io) => Some((io.minimum()..=io.maximum()).collect()),
        Resource::FixedIo(io) => Some(
            (0..u16::from(io.length()))
                .map(|i| io.address() + i)
                .collect(),
        ),
        _ => None,
    }
}

fn irq_list(resource: &Resource) -> Option<Vec<u32>> {
    match resource {
        Resource::Irq(irq) => Some(irq.irqs().iter().map(|&i| u32::from(i)).collect()),
        Resource::ExtendedIrq(irq) => Some(irq.irqs().to_vec()),
        _ => None,
    }
}

/// Returns the page-aligned base and the size of the memory view that covers a memory resource.
fn memory_view(memory: Memory) -> (usize, usize) {
    let aligned = (memory.address() as usize) & !PAGE_MASK;
    let page_off = (memory.address() as usize) & PAGE_MASK;
    let span = ((memory.length() as usize) + page_off + PAGE_MASK) & !PAGE_MASK;
    (aligned, span.max(PAGE_SIZE))
}

fn resolve_irq_to_gsi(irq: u32) -> u32 {
    #[cfg(target_arch = "x86_64")]
    {
        crate::isa::resolve_isa_irq(irq).gsi
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        irq
    }
}

impl AcpiObject for NodeObject {
    fn resources(&self) -> Option<AcpiResources> {
        let resources = self.node.current_resources().ok()?;

        let mut out = AcpiResources::default();
        for resource in resources.iter() {
            match &resource {
                Resource::Io(io) => out.io_ports.extend(io.minimum()..=io.maximum()),
                Resource::FixedIo(io) => out
                    .fixed_io_ports
                    .extend((0..u16::from(io.length())).map(|i| io.address() + i)),
                Resource::Memory(memory) => out.memory_ranges.push(AcpiMemoryRange {
                    address: memory.address(),
                    length: memory.length(),
                    offset: ((memory.address() as usize) & PAGE_MASK) as u32,
                }),
                Resource::Other(type_) => {
                    println!("sif: acpi: ignoring _CRS resource of type {type_}")
                }
                // IRQ resources are collected below.
                Resource::Irq(_) | Resource::ExtendedIrq(_) => (),
            }
            if let Some(irqs) = irq_list(&resource) {
                out.irqs.extend(irqs);
            }
        }
        Some(out)
    }

    fn access_ports(&self, index: usize) -> managarm::hw::Result<hel::Handle> {
        let resources = self
            .node
            .current_resources()
            .map_err(|_| HwError::DeviceError)?;

        let mut i = 0;
        for resource in resources.iter() {
            let Some(ports) = port_list(&resource) else {
                continue;
            };
            if i == index {
                let ports: Vec<usize> = ports.iter().map(|&port| usize::from(port)).collect();
                return Ok(hel::access_io(hardware_access_handle(), &ports)?);
            }
            i += 1;
        }
        Err(HwError::OutOfBounds)
    }

    fn access_irq(&self, index: usize) -> managarm::hw::Result<&hel::Handle> {
        let mut objects = self.irq_objects.lock().expect(EXPECT_LOCK);
        if let Some(&object) = objects.get(&index) {
            return Ok(object);
        }

        let resources = self
            .node
            .current_resources()
            .map_err(|_| HwError::DeviceError)?;

        let mut irqs = Vec::new();
        for resource in resources.iter() {
            if let Some(list) = irq_list(&resource) {
                irqs.extend(list);
            }
        }

        let &irq = irqs.get(index).ok_or(HwError::OutOfBounds)?;
        let gsi = resolve_irq_to_gsi(irq);
        let pin = hel::access_irq_by_gsi(hardware_access_handle(), u64::from(gsi))?;
        let object = leak(hel::handle_irq(&pin)?);
        objects.insert(index, object);
        Ok(object)
    }

    fn access_memory(&self, index: usize) -> managarm::hw::Result<hel::Handle> {
        let resources = self
            .node
            .current_resources()
            .map_err(|_| HwError::DeviceError)?;

        let memory = resources
            .iter()
            .filter_map(|resource| match resource {
                Resource::Memory(memory) => Some(memory),
                _ => None,
            })
            .nth(index)
            .ok_or(HwError::OutOfBounds)?;
        let (base, size) = memory_view(memory);
        Ok(hel::access_physical(
            hardware_access_handle(),
            base,
            size,
            hel::CachingMode::Mmio,
        )?)
    }

    fn dma_coherent(&self) -> bool {
        eval_cca(self.node)
    }
}

/// Publishes the acpi-object entity of a node and starts serving it.
pub async fn publish(node: NamespaceNode, instance: usize) -> Result<&'static EntityManager> {
    let path = node.absolute_path();

    let mut props = HashMap::new();
    props.insert("unix.subsystem".into(), string("acpi"));
    props.insert("acpi.path".into(), string(&path));
    if let Some(hid) = node.eval_hid().ok().flatten() {
        props.insert("acpi.hid".into(), string(&hid));
    }
    if let Some(cid) = node
        .eval_cid()
        .ok()
        .flatten()
        .and_then(|ids| ids.into_iter().next())
    {
        props.insert("acpi.cid".into(), string(&cid));
    }
    props.insert("acpi.instance".into(), string(&instance.to_string()));

    println!("sif: acpi: publishing object {path}");
    let manager = leak(create_entity("acpi-object", &props).await?);
    let object = Arc::new(NodeObject::new(node));
    hel::spawn(serve_entity_lanes(manager, move |lane| {
        hel::spawn(serve_acpi_object(lane, object.clone()));
    }));

    Ok(manager)
}

pub async fn publish_devices(hids: &[&CStr]) -> Result<()> {
    let mut nodes = Vec::new();
    namespace::find_devices_at(NamespaceNode::root(), hids, |node| {
        nodes.push(node);
        IterationDecision::Continue
    })?;

    for (instance, node) in nodes.into_iter().enumerate() {
        publish(node, instance).await?;
    }
    Ok(())
}
