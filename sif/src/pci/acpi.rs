use std::ffi::CStr;
use std::sync::atomic::Ordering;

use hel::{IrqPolarity, IrqTrigger};
use managarm::mbus::Properties;

use crate::acpi::object;
use crate::entity::{decimal, string};
use crate::leak;
use crate::uacpi::namespace::{
    IterationDecision, NamespaceNode, PredefinedNamespace, find_devices_at,
};
use crate::uacpi::pci::RoutingEntry as PrtEntry;
use crate::uacpi::resources::{Polarity, Resource, Triggering};
use crate::uacpi::runtime::{self, Aml};

use super::discover::add_root_bus;
use super::{
    IrqIndex, IrqPin, PciBus, PciError, PciIrqRouter, RouterState, RoutingEntry, RoutingModel,
    config, system_irq,
};

/// The ACPI node of a host bridge, which is published next to its root bus.
#[derive(Clone, Copy)]
pub struct HostBridge {
    node: NamespaceNode,
    uid: Option<u64>,
}

#[derive(Clone, Copy)]
struct RootBus {
    seg: u16,
    bus: u8,
    bridge: HostBridge,
}

/// Evaluates an integer method that the device may not implement.
fn eval_optional_integer(aml: Aml, node: NamespaceNode, path: &CStr) -> Option<u64> {
    match node.eval_simple_integer(aml, path) {
        Ok(value) => value,
        Err(err) => {
            println!("sif: Failed to evaluate {}: {err}", path.to_string_lossy());
            None
        }
    }
}

/// Evaluates an integer method that defaults to zero if the device does not implement it.
fn eval_integer_or_zero(aml: Aml, node: NamespaceNode, path: &CStr) -> u64 {
    eval_optional_integer(aml, node, path).unwrap_or(0)
}

/// Evaluates _CCA of a host bridge. Returns None if the coherency is unknown.
fn eval_cca(aml: Aml, node: NamespaceNode) -> Option<bool> {
    // Like Linux, ignore the firmware on x86 since all DMA snoops the CPU caches.
    if cfg!(target_arch = "x86_64") {
        return Some(true);
    }
    // OSPM ignores _CCA if an ancestor already has one, hence the outermost _CCA applies.
    let ancestors: Vec<NamespaceNode> = std::iter::successors(Some(node), |n| n.parent()).collect();
    for ancestor in ancestors.into_iter().rev() {
        match ancestor.eval_simple_integer(aml, c"_CCA") {
            Ok(Some(cca)) => return Some(cca != 0),
            Ok(None) => {}
            // Like Linux, treat a _CCA that fails to evaluate as absent.
            Err(err) => println!(
                "sif: Failed to evaluate _CCA of {}: {err}",
                ancestor.absolute_path()
            ),
        }
    }
    // Like Linux, only aarch64 requires _CCA; elsewhere DMA defaults to coherent.
    (!cfg!(target_arch = "aarch64")).then_some(true)
}

fn find_root_buses(aml: Aml) -> Vec<RootBus> {
    let mut roots: Vec<RootBus> = Vec::new();

    // PCIe host bridges usually declare PNP0A08 as their _HID and PNP0A03 as a _CID, hence
    // both IDs have to be matched in a single search to visit every bridge exactly once.
    let system_bus = NamespaceNode::predefined(PredefinedNamespace::SystemBus);
    let search = find_devices_at(aml, system_bus, &[c"PNP0A03", c"PNP0A08"], |node| {
        roots.push(RootBus {
            seg: eval_integer_or_zero(aml, node, c"_SEG") as u16,
            bus: eval_integer_or_zero(aml, node, c"_BBN") as u8,
            bridge: HostBridge {
                node,
                uid: eval_optional_integer(aml, node, c"_UID"),
            },
        });
        // Devices below a host bridge are never host bridges themselves.
        IterationDecision::NextPeer
    });
    if let Err(err) = search {
        println!("sif: Failed to search for PCI host bridges: {err}");
    }

    if roots.is_empty() {
        println!("sif: Firmware describes no PCI host bridge");
    }
    roots
}

/// Resolves the IRQ that a link device is connected to.
///
/// `index` is the source index of the _PRT entry, i.e., it selects a resource descriptor
/// of the _CRS of the link and not an IRQ within a descriptor.
fn resolve_link(
    aml: Aml,
    source: NamespaceNode,
    index: u32,
) -> Result<(u32, IrqTrigger, IrqPolarity), PciError> {
    let resources = source
        .current_resources(aml)
        .map_err(PciError::LinkResources)?;

    let resource = resources
        .iter()
        .nth(index as usize)
        .ok_or(PciError::LinkResourceMissing { index })?;

    let (gsi, triggering, polarity) = match resource {
        Resource::Irq(irq) => (
            irq.irqs().first().map(|&gsi| u32::from(gsi)),
            irq.triggering(),
            irq.polarity(),
        ),
        Resource::ExtendedIrq(irq) => (
            irq.irqs().first().copied(),
            irq.triggering(),
            irq.polarity(),
        ),
        _ => return Err(PciError::LinkResourceNotIrq { index }),
    };

    // Firmware reports a link that it did not connect to an IRQ as one without any IRQs.
    // TODO: Connect such links ourselves by picking an IRQ from _PRS and applying it via _SRS.
    let gsi = gsi.ok_or(PciError::LinkNotConnected)?;

    Ok((gsi, trigger_of(triggering), polarity_of(polarity)))
}

fn trigger_of(triggering: Triggering) -> IrqTrigger {
    match triggering {
        Triggering::Edge => IrqTrigger::Edge,
        Triggering::Level => IrqTrigger::Level,
    }
}

fn polarity_of(polarity: Polarity) -> IrqPolarity {
    match polarity {
        Polarity::ActiveHigh => IrqPolarity::High,
        Polarity::ActiveLow | Polarity::ActiveBoth => IrqPolarity::Low,
    }
}

/// A route of a _PRT, i.e., the GSI that an interrupt pin of a slot is connected to.
struct PrtRoute {
    slot: u8,
    index: IrqIndex,
    gsi: u32,
    triggering: IrqTrigger,
    polarity: IrqPolarity,
}

/// The outcome of evaluating the _PRT of a bus.
enum Prt {
    /// The bus has no _PRT.
    Missing,
    /// Evaluating the _PRT failed.
    Failed(PciError),
    Routes(Vec<PrtRoute>),
}

/// The ACPI node of a bus together with the outcome of evaluating its _PRT.
type AcpiBusNode = (NamespaceNode, Prt);

pub struct AcpiPciIrqRouter {
    state: RouterState,
    acpi_node: Option<NamespaceNode>,
}

impl AcpiPciIrqRouter {
    fn new(
        parent: Option<&'static dyn PciIrqRouter>,
        bus: &'static PciBus,
        acpi: Option<AcpiBusNode>,
    ) -> &'static AcpiPciIrqRouter {
        let acpi_node = acpi.as_ref().map(|&(node, _)| node);
        let mut state = RouterState::new();
        build_routing(&mut state, parent, bus, acpi.map(|(_, prt)| prt));

        leak(AcpiPciIrqRouter { state, acpi_node })
    }
}

fn evaluate_prt(aml: Aml, node: NamespaceNode) -> Prt {
    let pci_routes = match node.pci_routing_table(aml) {
        Ok(Some(pci_routes)) => pci_routes,
        Ok(None) => return Prt::Missing,
        Err(err) => return Prt::Failed(PciError::PrtEvaluation(err)),
    };

    // Walk through the PRT and determine the routing.
    let mut routes = Vec::new();
    for entry in pci_routes.entries() {
        let slot = ((entry.address >> 16) & 0xFFFF) as u8;
        match prt_route(aml, &entry) {
            Ok(route) => {
                if crate::TRACE.load(Ordering::Relaxed) {
                    println!(
                        "sif:     Route for slot {slot}, {}: GSI {}",
                        route.index.name(),
                        route.gsi
                    );
                }
                routes.push(route);
            }
            Err(err) => println!(
                "sif:     No route for slot {slot}, pin {}: {:#}",
                entry.pin,
                anyhow::Error::from(err)
            ),
        }
    }

    Prt::Routes(routes)
}

/// Determines the route that an entry of a _PRT describes.
fn prt_route(aml: Aml, entry: &PrtEntry) -> Result<PrtRoute, PciError> {
    // These are the defaults.
    let mut triggering = IrqTrigger::Level;
    let mut polarity = IrqPolarity::Low;
    let mut gsi = entry.index;
    let slot = ((entry.address >> 16) & 0xFFFF) as u8;

    // TODO: support routing of individual functions.
    if entry.address & 0xFFFF != 0xFFFF {
        return Err(PciError::FunctionRoute);
    }

    // _PRT pins are zero-based, unlike the interrupt pin register.
    let index = entry
        .pin
        .checked_add(1)
        .and_then(IrqIndex::from_pin)
        .ok_or(PciError::InvalidPin {
            pin: entry.pin.into(),
        })?;

    if let Some(source) = entry.source {
        (gsi, triggering, polarity) = resolve_link(aml, source, entry.index)?;
    }

    Ok(PrtRoute {
        slot,
        index,
        gsi,
        triggering,
        polarity,
    })
}

fn build_routing(
    state: &mut RouterState,
    parent: Option<&'static dyn PciIrqRouter>,
    bus: &'static PciBus,
    prt: Option<Prt>,
) {
    let Some(prt) = prt else {
        if let Some(parent) = parent {
            state.route_expansion_bridge(parent, bus);
        }
        return;
    };

    let routes = match prt {
        Prt::Routes(routes) => routes,
        Prt::Missing => {
            if let Some(parent) = parent {
                println!(
                    "sif: There is no _PRT for bus {}; assuming expansion bridge routing",
                    bus.bus_id
                );
                state.route_expansion_bridge(parent, bus);
            } else {
                println!(
                    "sif: There is no _PRT for bus {}; giving up IRQ routing of this bus",
                    bus.bus_id
                );
            }
            return;
        }
        Prt::Failed(err) => {
            println!(
                "sif: Giving up IRQ routing of bus {}: {:#}",
                bus.bus_id,
                anyhow::Error::from(err)
            );
            return;
        }
    };

    for route in routes {
        let pin = match system_irq(route.gsi, route.triggering, route.polarity) {
            Ok(pin) => pin,
            Err(err) => {
                println!(
                    "sif: Failed to set up GSI {} for slot {}, {}: {err}",
                    route.gsi,
                    route.slot,
                    route.index.name()
                );
                continue;
            }
        };
        state.routing_table.push(RoutingEntry {
            slot: route.slot,
            index: route.index,
            pin,
        });
    }

    state.routing_model = RoutingModel::RootTable;
}

/// Searches the ACPI node of the bridge whose _ADR is bridge_adr.
fn find_bridge_node(aml: Aml, parent: NamespaceNode, bridge_adr: u64) -> Option<NamespaceNode> {
    let mut node = None;
    let mut match_adr = |candidate: NamespaceNode| match candidate.eval_adr(aml) {
        Ok(Some(adr)) if adr == bridge_adr => {
            node = Some(candidate);
            IterationDecision::Break
        }
        _ => IterationDecision::Continue,
    };

    if let Err(err) = parent.for_each_child_device(aml, &mut match_adr) {
        println!("sif: Failed to search for the ACPI node of a PCI bridge: {err}");
    }
    node
}

impl PciIrqRouter for AcpiPciIrqRouter {
    fn resolve_irq_route(&self, slot: u8, index: IrqIndex) -> Option<&'static IrqPin> {
        self.state.resolve_irq_route(slot, index)
    }

    fn make_downstream_router(&'static self, bus: &'static PciBus) -> &'static dyn PciIrqRouter {
        let bridge = bus
            .associated_bridge
            .expect("downstream router without an associated bridge");
        let bridge_adr = ((bridge.entity.slot as u64) << 16) | bridge.entity.function as u64;

        // Enumeration is synchronous, hence it has to block on AML.
        let acpi = self.acpi_node.and_then(|acpi_node| {
            runtime::run_blocking(move |aml| {
                let node = find_bridge_node(aml, acpi_node, bridge_adr)?;
                Some((node, evaluate_prt(aml, node)))
            })
        });

        AcpiPciIrqRouter::new(Some(self), bus, acpi)
    }
}

pub async fn discover_root_buses() {
    if !crate::acpi::has_rsdp() {
        return;
    }

    for root in runtime::run(find_root_buses).await {
        let Some(io) = config::get_config_io_for(root.seg, root.bus) else {
            println!(
                "sif: No config space for PCI host bridge {:04x}:{:02x}",
                root.seg, root.bus
            );
            continue;
        };

        println!(
            "sif: Found PCI host bridge {:04x}:{:02x}",
            root.seg, root.bus
        );

        let node = root.bridge.node;
        let (prt, dma_coherent) =
            runtime::run(move |aml| (evaluate_prt(aml, node), eval_cca(aml, node))).await;
        let root_bus = PciBus::new(None, io, root.seg, root.bus, dma_coherent);
        let router = AcpiPciIrqRouter::new(None, root_bus, Some((node, prt)));
        assert!(
            root_bus.irq_router.set(router).is_ok(),
            "sif: PCI bus already has an IRQ router"
        );
        assert!(
            root_bus.host_bridge.set(root.bridge).is_ok(),
            "sif: PCI bus already has a host bridge"
        );
        add_root_bus(root_bus);
    }
}

/// Publishes the acpi-object of the host bridge of a published root bus.
///
/// UEFI device paths start at the host bridge, hence they resolve through this object.
pub async fn publish_host_bridge(root_bus: &'static PciBus) -> anyhow::Result<()> {
    let Some(&HostBridge { node, uid }) = root_bus.host_bridge.get() else {
        return Ok(());
    };

    let mut props = Properties::new();
    if let Some(uid) = uid {
        props.insert("acpi.uid".into(), string(&uid.to_string()));
    }
    let mbus_id = root_bus.mbus_id.load(Ordering::Relaxed);
    props.insert("acpi.physical_node".into(), decimal(mbus_id));

    // Device paths name host bridges by _UID, which the devserver takes from the instance.
    object::publish_with_properties(node, uid.unwrap_or(0) as usize, props).await?;
    Ok(())
}
