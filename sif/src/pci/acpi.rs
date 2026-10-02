use std::ffi::CStr;

use hel::{IrqPolarity, IrqTrigger};

use crate::acpi::eval_cca;
use crate::uacpi::namespace::{
    IterationDecision, NamespaceNode, PredefinedNamespace, find_devices_at,
};
use crate::uacpi::resources::{Polarity, Resource, Triggering};

use super::discover::add_root_bus;
use super::{
    IrqIndex, IrqPin, PciBus, PciIrqRouter, RouterState, RoutingEntry, RoutingModel, config, leak,
    system_irq,
};

#[derive(Clone, Copy)]
struct RootBus {
    seg: u16,
    bus: u8,
    node: NamespaceNode,
}

/// Evaluates an integer method that defaults to zero if the device does not implement it.
fn eval_integer_or_zero(node: NamespaceNode, path: &CStr) -> u64 {
    match node.eval_simple_integer(path) {
        Ok(value) => value.unwrap_or(0),
        Err(err) => {
            println!("sif: Failed to evaluate {}: {err}", path.to_string_lossy());
            0
        }
    }
}

fn find_root_buses() -> Vec<RootBus> {
    let mut roots: Vec<RootBus> = Vec::new();

    // PCIe host bridges usually declare PNP0A08 as their _HID and PNP0A03 as a _CID, hence
    // both IDs have to be matched in a single search to visit every bridge exactly once.
    let system_bus = NamespaceNode::predefined(PredefinedNamespace::SystemBus);
    let search = find_devices_at(system_bus, &[c"PNP0A03", c"PNP0A08"], |node| {
        roots.push(RootBus {
            seg: eval_integer_or_zero(node, c"_SEG") as u16,
            bus: eval_integer_or_zero(node, c"_BBN") as u8,
            node,
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
fn resolve_link(source: NamespaceNode, index: u32) -> Option<(u32, IrqTrigger, IrqPolarity)> {
    let resources = match source.current_resources() {
        Ok(resources) => resources,
        Err(err) => {
            println!("sif: Failed to evaluate the _CRS of an IRQ link: {err}");
            return None;
        }
    };

    let Some(resource) = resources.iter().nth(index as usize) else {
        println!("sif: The _CRS of an IRQ link has no resource {index}");
        return None;
    };

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
        _ => {
            println!("sif: Resource {index} of an IRQ link does not describe an IRQ");
            return None;
        }
    };

    // Firmware reports a link that it did not connect to an IRQ as one without any IRQs.
    // TODO: Connect such links ourselves by picking an IRQ from _PRS and applying it via _SRS.
    let Some(gsi) = gsi else {
        println!("sif: An IRQ link is not connected to any IRQ");
        return None;
    };

    Some((gsi, trigger_of(triggering), polarity_of(polarity)))
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

pub struct AcpiPciIrqRouter {
    state: RouterState,
    acpi_node: Option<NamespaceNode>,
}

impl AcpiPciIrqRouter {
    pub fn new(
        parent: Option<&'static dyn PciIrqRouter>,
        bus: &'static PciBus,
        node: Option<NamespaceNode>,
    ) -> &'static AcpiPciIrqRouter {
        let mut state = RouterState::new();
        build_routing(&mut state, parent, bus, node);

        leak(AcpiPciIrqRouter {
            state,
            acpi_node: node,
        })
    }
}

fn build_routing(
    state: &mut RouterState,
    parent: Option<&'static dyn PciIrqRouter>,
    bus: &'static PciBus,
    node: Option<NamespaceNode>,
) {
    let Some(node) = node else {
        if let Some(parent) = parent {
            state.route_expansion_bridge(parent, bus);
        }
        return;
    };

    let pci_routes = match node.pci_routing_table() {
        Ok(Some(pci_routes)) => pci_routes,
        Ok(None) => {
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
        Err(err) => {
            println!("sif: Failed to evaluate _PRT: {err}; giving up IRQ routing");
            return;
        }
    };

    // Walk through the PRT and determine the routing.
    for entry in pci_routes.entries() {
        // These are the defaults.
        let mut triggering = IrqTrigger::Level;
        let mut polarity = IrqPolarity::Low;
        let mut gsi = entry.index;
        let slot = ((entry.address >> 16) & 0xFFFF) as u8;

        assert!(
            entry.address & 0xFFFF == 0xFFFF,
            "TODO: support routing of individual functions"
        );

        let index = IrqIndex::from_pin(entry.pin + 1);

        if let Some(source) = entry.source {
            match resolve_link(source, entry.index) {
                Some((link_gsi, link_trigger, link_polarity)) => {
                    gsi = link_gsi;
                    triggering = link_trigger;
                    polarity = link_polarity;
                }
                None => {
                    println!("sif:     No route for slot {slot}, {}", index.name());
                    continue;
                }
            }
        }

        println!(
            "sif:     Route for slot {slot}, {}: GSI {gsi}",
            index.name()
        );

        let Some(pin) = system_irq(gsi, triggering, polarity) else {
            continue;
        };
        state.routing_table.push(RoutingEntry { slot, index, pin });
    }

    state.routing_model = RoutingModel::RootTable;
}

/// Searches the ACPI node of the bridge that a bus is attached to.
fn find_bridge_node(parent: NamespaceNode, bus: &PciBus) -> Option<NamespaceNode> {
    let bridge = bus
        .associated_bridge
        .expect("downstream router without an associated bridge");
    let bridge_adr = ((bridge.entity.slot as u64) << 16) | bridge.entity.function as u64;

    let mut node = None;
    let mut match_adr = |candidate: NamespaceNode| match candidate.eval_adr() {
        Ok(Some(adr)) if adr == bridge_adr => {
            node = Some(candidate);
            IterationDecision::Break
        }
        _ => IterationDecision::Continue,
    };

    if let Err(err) = parent.for_each_child_device(&mut match_adr) {
        println!("sif: Failed to search for the ACPI node of a PCI bridge: {err}");
    }
    node
}

impl PciIrqRouter for AcpiPciIrqRouter {
    fn resolve_irq_route(&self, slot: u8, index: IrqIndex) -> Option<&'static IrqPin> {
        self.state.resolve_irq_route(slot, index)
    }

    fn make_downstream_router(&'static self, bus: &'static PciBus) -> &'static dyn PciIrqRouter {
        let node = self
            .acpi_node
            .and_then(|acpi_node| find_bridge_node(acpi_node, bus));

        AcpiPciIrqRouter::new(Some(self), bus, node)
    }
}

pub fn discover_root_buses() {
    if !crate::acpi::has_rsdp() {
        return;
    }

    for root in find_root_buses() {
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

        let root_bus = PciBus::new(None, io, root.seg, root.bus, eval_cca(root.node));
        let router = AcpiPciIrqRouter::new(None, root_bus, Some(root.node));
        assert!(
            root_bus.irq_router.set(router).is_ok(),
            "sif: PCI bus already has an IRQ router"
        );
        add_root_bus(root_bus);
    }
}
