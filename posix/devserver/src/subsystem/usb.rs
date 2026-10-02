//! The usb bus subsystem: host controllers (as root hubs), devices,
//! interfaces and endpoints, plus the driver links of bound interfaces.

use std::sync::{Arc, Mutex};

use anyhow::{Context, Result, anyhow, bail};
use managarm::mbus;
use managarm::usb::{self, descriptor};

use crate::EXPECT_LOCK;
use crate::device::{
    AttrGroup, DevNode, DevNodeSpec, DeviceKey, DeviceSpec, IdAllocator, Membership, Model,
    NodeType, Parent, Role,
};
use crate::subsystem::{
    Installed, acpi, attr, hex_prop, interface_key, observe, parse_prop, remote_lane,
    required_prop, string_prop,
};
use crate::sysfs::StaticAttribute;

const USB_MAJOR: i32 = 189;

/// The descriptors that the root hub of a controller reports.
mod root_hub {
    fn device_descriptor(
        bcd_usb: u16,
        protocol: u8,
        max_packet_size0: u8,
        product: u8,
    ) -> [u8; 18] {
        let [usb_lo, usb_hi] = bcd_usb.to_le_bytes();
        [
            0x12,
            0x01,
            usb_lo,
            usb_hi,
            0x09,
            0x00,
            protocol,
            max_packet_size0,
            0x6b,
            0x1d,
            product,
            0x00,
            0x00,
            0x00,
            0x03,
            0x02,
            0x01,
            0x01,
        ]
    }

    pub fn usb1_1() -> [u8; 18] {
        device_descriptor(0x0110, 0x00, 0x40, 0x01)
    }

    pub fn usb2_0() -> [u8; 18] {
        device_descriptor(0x0200, 0x00, 0x40, 0x02)
    }

    pub fn usb3_0() -> [u8; 18] {
        device_descriptor(0x0300, 0x03, 0x09, 0x03)
    }

    pub fn usb3_1() -> [u8; 18] {
        device_descriptor(0x0310, 0x03, 0x09, 0x03)
    }

    // One hub interface with one interrupt IN endpoint.
    fn configuration(total_length: u8, max_packet_size: u8, interval: u8) -> [u8; 25] {
        [
            0x09,
            0x02,
            total_length,
            0x00,
            0x01,
            0x01,
            0x00,
            0xC0,
            0x00, //
            0x09,
            0x04,
            0x00,
            0x00,
            0x01,
            0x09,
            0x00,
            0x00,
            0x00, //
            0x07,
            0x05,
            0x81,
            0x03,
            max_packet_size,
            0x00,
            interval,
        ]
    }

    pub fn full_speed() -> Vec<u8> {
        configuration(0x19, 0x02, 0xFF).to_vec()
    }

    pub fn high_speed() -> Vec<u8> {
        configuration(0x19, 0x04, 0x0c).to_vec()
    }

    pub fn super_speed() -> Vec<u8> {
        let mut desc = configuration(0x1f, 0x04, 0x0c).to_vec();
        // SuperSpeed endpoint companion descriptor.
        desc.extend_from_slice(&[0x06, 0x30, 0x00, 0x00, 0x02, 0x00]);
        desc
    }
}

/// The data of a controller (root hub) device.
struct Controller {
    bus: u32,
}

struct State {
    model: Arc<Model>,
    bus_ids: Mutex<IdAllocator>,
}

fn devnode(bus: u32, devnum: u32) -> DevNodeSpec {
    DevNodeSpec::Foreign(DevNode {
        path: format!("bus/usb/{bus:03}/{devnum:03}"),
        node_type: NodeType::Char,
        major: USB_MAJOR,
        minor: ((bus - 1) * 128 + devnum - 1) as i32,
    })
}

fn product(desc: &descriptor::DeviceDescriptor) -> String {
    format!(
        "{:x}:{:x}:{:x}",
        desc.id_vendor, desc.id_product, desc.bcd_device
    )
}

/// The attributes that controllers (root hubs) and devices share.
fn add_common_attrs(
    spec: DeviceSpec,
    desc: &descriptor::DeviceDescriptor,
    speed: &str,
    bus: u32,
    devnum: u32,
    descriptors: &[u8],
    max_power: u32,
    num_interfaces: u8,
) -> DeviceSpec {
    let attrs = vec![
        attr("idVendor", format!("{:04x}\n", desc.id_vendor)),
        attr("idProduct", format!("{:04x}\n", desc.id_product)),
        attr("bDeviceClass", format!("{:02x}\n", desc.device_class)),
        attr("bDeviceSubClass", format!("{:02x}\n", desc.device_subclass)),
        attr("bDeviceProtocol", format!("{:02x}\n", desc.device_protocol)),
        attr(
            "version",
            format!("{:>2x}.{:02x}\n", desc.bcd_usb >> 8, desc.bcd_usb & 0xff),
        ),
        attr("speed", format!("{speed}\n")),
        attr("bMaxPower", format!("{max_power}mA\n")),
        attr("maxchild", "2\n"),
        attr("bNumInterfaces", format!("{num_interfaces:>2}\n")),
        attr("busnum", format!("{bus}\n")),
        attr("devnum", format!("{devnum}\n")),
        attr("rx_lanes", "1\n"),
        attr("tx_lanes", "1\n"),
    ];
    spec.attrs(attrs).attr(
        "descriptors",
        StaticAttribute::new_sized(descriptors.to_vec()),
    )
}

fn endpoint_group(ep: &descriptor::EndpointDescriptor) -> AttrGroup {
    let group = AttrGroup::named(format!("ep_{:02x}", ep.endpoint_address & 0x8f));
    let ep_type = match ep.attributes & 0x03 {
        0 => "Control",
        1 => "Isochronous",
        2 => "Bulk",
        _ => "Interrupt",
    };
    let attrs = vec![
        attr("bEndpointAddress", format!("{:02x}\n", ep.endpoint_address)),
        attr("interval", "0ms\n"),
        attr("bInterval", format!("{:02x}\n", ep.interval)),
        attr("bLength", format!("{:02x}\n", ep.length)),
        attr("bmAttributes", format!("{:02x}\n", ep.attributes)),
        attr("wMaxPacketSize", format!("{:04x}\n", ep.max_packet_size)),
        attr("type", format!("{ep_type}\n")),
    ];
    group.attrs(attrs)
}

/// The name of the controller, which its root hub reports as serial: the PCI address of a PCI
/// controller or the device name of an ACPI one.
async fn controller_name(parent_id: i64) -> Result<String> {
    let properties = mbus::Entity::from_id(parent_id)
        .get_properties()
        .await
        .map_err(|e| anyhow!("failed to query the controller properties: {e}"))?;
    if string_prop(&properties, "acpi.hid").is_some() {
        return Ok(format!("{}\n", acpi::device_name(&properties)?));
    }
    Ok(format!(
        "{:04x}:{:02x}:{:02x}.{:x}\n",
        hex_prop(&properties, "pci-segment")?,
        hex_prop(&properties, "pci-bus")?,
        hex_prop(&properties, "pci-slot")?,
        hex_prop(&properties, "pci-function")?
    ))
}

async fn install_controller(
    state: Arc<State>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let mbus_id = event.entity_id();
    let parent: i64 = parse_prop(properties, "usb.root.parent")?;
    let controller_type = required_prop(properties, "generic.devsubtype")?;
    let major: u32 = parse_prop(properties, "usb.version.major")?;
    let minor: u32 = parse_prop(properties, "usb.version.minor")?;

    let (speed, mut descriptors, config) = match (major, minor) {
        (1, 0) => ("1.5", root_hub::usb1_1().to_vec(), root_hub::full_speed()),
        (1, 0x10) => ("12", root_hub::usb1_1().to_vec(), root_hub::full_speed()),
        (2, _) => ("480", root_hub::usb2_0().to_vec(), root_hub::high_speed()),
        (3, 0) => ("5000", root_hub::usb3_0().to_vec(), root_hub::super_speed()),
        (3, 0x10) => (
            "10000",
            root_hub::usb3_1().to_vec(),
            root_hub::super_speed(),
        ),
        (3, 0x20) => (
            "20000",
            root_hub::usb3_1().to_vec(),
            root_hub::super_speed(),
        ),
        _ => bail!("unsupported USB version {major}.{minor:x}"),
    };
    descriptors.extend_from_slice(&config);
    let desc =
        descriptor::DeviceDescriptor::parse(&descriptors).context("bad root hub descriptor")?;

    let bus = state.bus_ids.lock().expect(EXPECT_LOCK).allocate();
    let name = format!("usb{bus}");
    println!("devserver: Installing USB controller {name} (mbus ID: {mbus_id})");

    // The controller is published before its parent device in sif mode; wait for it.
    let parent_device = state.model.wait_device(&DeviceKey::primary(parent)).await;
    let serial = controller_name(parent).await?;

    let spec = DeviceSpec::new(name, Parent::Device(parent_device), Membership::bus("usb"))
        .key(DeviceKey::primary(mbus_id))
        .devtype("usb_device")
        .devnode(devnode(bus, 1))
        .uevent_extra("PRODUCT", product(&desc))
        .uevent_extra("BUSNUM", format!("{bus:03}"))
        .uevent_extra("DEVNUM", "001")
        .data(Controller { bus });
    let mut spec = add_common_attrs(spec, &desc, speed, bus, 1, &descriptors, 0, 1)
        .attr("manufacturer", StaticAttribute::new("managarm\n"));
    let product_name = match controller_type {
        "xhci" => Some("xHCI Host Controller\n"),
        "ehci" => Some("EHCI Host Controller\n"),
        "uhci" => Some("UHCI Host Controller\n"),
        _ => None,
    };
    if let Some(product_name) = product_name {
        spec = spec.attr("product", StaticAttribute::new(product_name));
    }
    let spec = spec
        .attr("serial", StaticAttribute::new(serial))
        .group(endpoint_group(&descriptor::EndpointDescriptor {
            length: 7,
            endpoint_address: 0,
            attributes: 0,
            max_packet_size: desc.max_packet_size0 as u16,
            interval: 0,
        }));
    let device = state.model.create_and_announce(spec).await?;

    hel::spawn(crate::log_subsystem_errors(
        "usb-devices",
        observe_devices(state, mbus_id),
    ));
    Ok(vec![Box::new(device)])
}

async fn observe_devices(state: Arc<State>, controller_id: i64) -> Result<()> {
    let controller = controller_id.to_string();
    let filters = [
        mbus::Filter::Equals("unix.subsystem", "usb"),
        mbus::Filter::Equals("usb.type", "device"),
        mbus::Filter::Equals("usb.bus", &controller),
    ];
    observe(mbus::Filter::Conjunction(&filters), |scope, event| {
        scope.install(
            format!("USB device {}", event.entity_id()),
            install_device(state.clone(), event),
        );
    })
    .await
}

async fn install_device(
    state: Arc<State>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let mbus_id = event.entity_id();
    let controller_id: i64 = parse_prop(properties, "usb.bus")?;
    let port = hex_prop(properties, "usb.hub_port")?;
    let speed = required_prop(properties, "usb.speed")?;

    let controller = state
        .model
        .wait_device(&DeviceKey::primary(controller_id))
        .await;
    let bus = controller
        .data::<Controller>()
        .with_context(|| format!("{} is not a USB controller", controller.dir().sysfs_path()))?
        .bus;
    let devnum = port + 1;
    let name = format!("{bus}-{port}");
    println!("devserver: Installing USB device {name} (mbus ID: {mbus_id})");

    let hw = usb::Device::new(remote_lane(&event).await?);
    let mut descriptors = hw.device_descriptor().await?;
    let desc =
        descriptor::DeviceDescriptor::parse(&descriptors).context("bad device descriptor")?;
    let config_value = hw.current_configuration_value().await?;

    // The descriptors attribute carries every configuration; interfaces come
    // from the active one.
    let mut active = None;
    for index in 0..desc.num_configurations {
        let config = hw.configuration_descriptor(index).await?;
        if descriptor::config_descriptor(&config).map(|c| c.config_value) == Some(config_value) {
            active = Some(config.clone());
        }
        descriptors.extend_from_slice(&config);
    }
    // An unconfigured device still appears in sysfs, just without interfaces.
    let (config_desc, interfaces) = match &active {
        Some(config) => (
            descriptor::config_descriptor(config),
            descriptor::interfaces(config),
        ),
        None => {
            println!("devserver: USB device {name} has no active configuration");
            (None, Vec::new())
        }
    };
    let config_desc = config_desc.unwrap_or_default();

    let spec = DeviceSpec::new(
        name.clone(),
        Parent::Device(controller),
        Membership::bus("usb"),
    )
    .key(DeviceKey::primary(mbus_id))
    .devtype("usb_device")
    .devnode(devnode(bus, devnum))
    .uevent_extra("PRODUCT", product(&desc))
    .uevent_extra("BUSNUM", format!("{bus:03}"))
    .uevent_extra("DEVNUM", format!("{devnum:03}"));
    let mut spec = add_common_attrs(
        spec,
        &desc,
        speed,
        bus,
        devnum,
        &descriptors,
        config_desc.max_power as u32 * 2,
        config_desc.num_interfaces,
    )
    .attr(
        "bcdDevice",
        StaticAttribute::new(format!("{:04x}\n", desc.bcd_device)),
    );
    // String descriptors are optional; only the ones the device has become attributes.
    for (attr_name, index) in [
        ("manufacturer", desc.manufacturer),
        ("product", desc.product),
        ("serial", desc.serial_number),
    ] {
        if let Ok(value) = hw.string(index).await {
            spec = spec.attr(attr_name, StaticAttribute::new(format!("{value}\n")));
        }
    }
    let attrs = vec![
        attr("bConfigurationValue", format!("{config_value}\n")),
        attr("bMaxPacketSize0", format!("{}\n", desc.max_packet_size0)),
        attr("configuration", "\n"),
        attr(
            "bmAttributes",
            format!("{:2x}\n", config_desc.bm_attributes),
        ),
        attr(
            "bNumConfigurations",
            format!("{}\n", desc.num_configurations),
        ),
    ];
    let spec = spec
        .attrs(attrs)
        .group(endpoint_group(&descriptor::EndpointDescriptor {
            length: 7,
            endpoint_address: 0,
            attributes: 0,
            max_packet_size: desc.max_packet_size0 as u16,
            interval: 0,
        }));
    let device = state.model.create_and_announce(spec).await?;

    let mut installed: Vec<Box<dyn Installed>> = Vec::new();
    for interface in &interfaces {
        let intf = &interface.descriptor;
        // Only the default alternate setting is represented.
        if intf.alternate_setting != 0 {
            continue;
        }
        let if_name = format!(
            "{name}:{}.{}",
            config_desc.config_value, intf.interface_number
        );
        let mut spec = DeviceSpec::new(
            if_name,
            Parent::Device(device.device().clone()),
            Membership::bus("usb"),
        )
        .key(DeviceKey::new(
            mbus_id,
            Role::UsbInterface {
                config: config_desc.config_value,
                number: intf.interface_number,
            },
        ))
        .devtype("usb_interface")
        .uevent_extra("PRODUCT", product(&desc))
        .uevent_extra(
            "INTERFACE",
            format!(
                "{}/{}/{}",
                intf.interface_class, intf.interface_subclass, intf.interface_protocol
            ),
        );
        let attrs = vec![
            attr("bInterfaceClass", format!("{:02x}\n", intf.interface_class)),
            attr(
                "bInterfaceSubClass",
                format!("{:02x}\n", intf.interface_subclass),
            ),
            attr(
                "bInterfaceProtocol",
                format!("{:02x}\n", intf.interface_protocol),
            ),
            attr(
                "bAlternateSetting",
                format!("{:>2x}\n", intf.alternate_setting),
            ),
            attr(
                "bInterfaceNumber",
                format!("{:02x}\n", intf.interface_number),
            ),
            attr("bNumEndpoints", format!("{:02x}\n", intf.num_endpoints)),
        ];
        spec = spec.attrs(attrs);
        for ep in &interface.endpoints {
            spec = spec.group(endpoint_group(ep));
        }
        installed.push(Box::new(state.model.create_and_announce(spec).await?));
    }

    hel::spawn(crate::log_subsystem_errors(
        "usb-drivers",
        observe_children(state, mbus_id),
    ));
    installed.push(Box::new(device));
    Ok(installed)
}

/// Watches the children of a USB device for `usb.interface_drivers`, which
/// names the driver bound to each interface, and binds the interfaces to
/// their driver objects.
async fn observe_children(state: Arc<State>, device_id: i64) -> Result<()> {
    let parent = device_id.to_string();
    let mut enumerator =
        mbus::Enumerator::new(mbus::Filter::Equals("drvcore.mbus-parent", &parent));
    loop {
        let (_, events) = enumerator
            .next_events()
            .await
            .map_err(|e| anyhow!("mbus enumeration failed: {e}"))?;
        for event in events {
            let Some(mbus::Item::Array(drivers)) = event.properties().get("usb.interface_drivers")
            else {
                continue;
            };
            for entry in drivers {
                let mbus::Item::Array(pair) = entry else {
                    continue;
                };
                let [mbus::Item::String(interface), mbus::Item::String(driver)] = pair.as_slice()
                else {
                    continue;
                };
                let Some(key) = interface_key(device_id, interface) else {
                    continue;
                };
                let Some(if_device) = state.model.lookup(&key) else {
                    eprintln!("devserver: driver {driver} bound to unknown interface {key:?}");
                    continue;
                };
                // A failure must not end the driver binding of the other interfaces.
                if let Err(e) = state.model.bind_driver(&if_device, driver).await {
                    eprintln!("devserver: failed to bind {driver} to {key:?}: {e:#}");
                }
            }
        }
    }
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    // The bus directory exists even before the first device shows up.
    model.subsystem(&Membership::bus("usb"))?;
    let state = Arc::new(State {
        model,
        bus_ids: Mutex::new(IdAllocator::new(1)),
    });

    observe(
        mbus::Filter::Equals("generic.devtype", "usb-controller"),
        |scope, event| {
            scope.install(
                format!("USB controller {}", event.entity_id()),
                install_controller(state.clone(), event),
            );
        },
    )
    .await
}
