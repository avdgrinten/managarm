//! Publishes the PS/2 devices of the ACPI namespace; port of thor's system/acpi/ps2.cpp.

use std::collections::HashMap;
use std::ffi::CStr;

use anyhow::Result;
use managarm::mbus::create_entity;

use crate::acpi::object::publish_devices;
use crate::entity::{dismiss_requests, serve_entity_lanes, string};
use crate::leak;

const ACPI_HID_PS2_KEYBOARDS: &[&CStr] = &[
    c"PNP0300", c"PNP0301", c"PNP0302", c"PNP0303", c"PNP0304", c"PNP0305", c"PNP0306", c"PNP0307",
    c"PNP0308", c"PNP0309", c"PNP030A", c"PNP030B", c"PNP0320", c"PNP0321", c"PNP0322", c"PNP0323",
    c"PNP0324", c"PNP0325", c"PNP0326", c"PNP0327", c"PNP0340", c"PNP0341", c"PNP0342", c"PNP0343",
    c"PNP0344",
];

const ACPI_HID_PS2_MICE: &[&CStr] = &[
    c"PNP0F00", c"PNP0F01", c"PNP0F02", c"PNP0F03", c"PNP0F04", c"PNP0F05", c"PNP0F06", c"PNP0F07",
    c"PNP0F08", c"PNP0F09", c"PNP0F0A", c"PNP0F0B", c"PNP0F0C", c"PNP0F0D", c"PNP0F0E", c"PNP0F0F",
    c"PNP0F10", c"PNP0F11", c"PNP0F12", c"PNP0F13", c"PNP0F14", c"PNP0F15", c"PNP0F16", c"PNP0F17",
    c"PNP0F18", c"PNP0F19", c"PNP0F1A", c"PNP0F1B", c"PNP0F1C", c"PNP0F1D", c"PNP0F1E", c"PNP0F1F",
    c"PNP0F20", c"PNP0F21", c"PNP0F22", c"PNP0F23", c"PNP0FFC", c"PNP0FFF",
];

// Notifies listeners that all PS/2 objects of the ACPI namespace have been published,
// so that they can stop running mbus filters indefinitely.
async fn publish_status() -> Result<()> {
    let mut props = HashMap::new();
    props.insert("unix.subsystem".into(), string("acpi"));
    props.insert("acpi.status".into(), string("ps2.init_complete"));

    let manager = leak(create_entity("acpi-status", &props).await?);
    hel::spawn(serve_entity_lanes(manager, |lane| {
        hel::spawn(dismiss_requests(lane));
    }));

    Ok(())
}

/// Publishes the acpi-object entities of all PS/2 keyboards and mice.
pub async fn publish() -> Result<()> {
    publish_devices(ACPI_HID_PS2_KEYBOARDS).await?;
    publish_devices(ACPI_HID_PS2_MICE).await?;

    publish_status().await?;

    Ok(())
}
