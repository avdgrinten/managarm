//! Publishes the USB host controllers that the ACPI namespace describes as platform devices.

use std::ffi::CStr;

use anyhow::Result;

use crate::acpi::object::publish_devices;

/// xHCI controllers, without (PNP0D10) and with (PNP0D15) a debug capability.
const ACPI_HID_XHCI: &[&CStr] = &[c"PNP0D10", c"PNP0D15"];

/// Publishes the acpi-object entities of all xHCI controllers.
pub async fn publish() -> Result<()> {
    // Search for both IDs at once since a controller may list one as _HID and the other as _CID.
    publish_devices(ACPI_HID_XHCI).await
}
