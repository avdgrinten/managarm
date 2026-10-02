//! The acpi bus subsystem: ACPI namespace objects with a hardware id.

use std::sync::Arc;

use anyhow::Result;
use managarm::mbus;

use crate::device::{DeviceKey, DeviceSpec, Membership, Model, Parent};
use crate::subsystem::{Installed, attr, observe, parse_prop, required_prop, string_prop};

/// The device name of an ACPI object, i.e., its _HID followed by its instance number.
pub fn device_name(properties: &mbus::Properties) -> Result<String> {
    let hid = required_prop(properties, "acpi.hid")?;
    let instance: u32 = parse_prop(properties, "acpi.instance")?;
    Ok(format!("{hid}:{instance:02}"))
}

async fn install_entity(
    model: Arc<Model>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let mbus_id = event.entity_id();
    let hid = required_prop(properties, "acpi.hid")?;
    let path = required_prop(properties, "acpi.path")?;
    let physical_node: Option<i64> = match string_prop(properties, "acpi.physical_node") {
        Some(_) => Some(parse_prop(properties, "acpi.physical_node")?),
        None => None,
    };

    let mut attrs = vec![
        attr("hid", format!("{hid}\n")),
        attr("path", format!("{path}\n")),
    ];
    if let Some(uid) = string_prop(properties, "acpi.uid") {
        attrs.push(attr("uid", format!("{uid}\n")));
    }

    let spec = DeviceSpec::new(
        device_name(properties)?,
        Parent::None,
        Membership::bus("acpi"),
    )
    .key(DeviceKey::primary(mbus_id))
    .attrs(attrs);
    let device = model.create_and_announce(spec).await?;

    // The physical device that this object describes is installed by another
    // subsystem (usually pci) and may appear later.
    let mut relation = None;
    if let Some(physical_node) = physical_node {
        let target = model.wait_device(&DeviceKey::primary(physical_node)).await;
        relation = Some(model.relate(&device, "physical_node", &target, Some("firmware_node"))?);
    }
    let mut installed: Vec<Box<dyn Installed>> = vec![Box::new(device)];
    if let Some(relation) = relation {
        installed.push(Box::new(relation));
    }
    Ok(installed)
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    observe(
        mbus::Filter::Equals("unix.subsystem", "acpi"),
        |scope, event| {
            if string_prop(event.properties(), "acpi.hid").is_none() {
                return;
            }
            scope.install(
                format!("ACPI object {}", event.entity_id()),
                install_entity(model.clone(), event),
            );
        },
    )
    .await
}
