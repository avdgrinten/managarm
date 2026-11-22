//! Per-subsystem mbus enumeration loops that populate the device model.
//!
//! Every loop follows the same shape: [`observe()`] an mbus filter, and install
//! each created entity on its own task through [`Scope::install()`] so that
//! one entity waiting for its parent (or failing) never stalls or kills the
//! rest of the subsystem. Each loop owns what its entities installed.

pub mod acpi;
pub mod block;
pub mod dmi;
pub mod drm;
#[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
pub mod dt;
pub mod generic;
pub mod graphics;
pub mod input;
pub mod kvm;
pub mod net;
pub mod nvme;
pub mod pci;
pub mod power_supply;
pub mod sound;
pub mod tty;
pub mod usb;
pub mod usbmisc;

use std::collections::BTreeMap;
use std::str::FromStr;
use std::sync::{Arc, Mutex};

use anyhow::{Context, Result, anyhow, bail};
use managarm::mbus;

use crate::EXPECT_LOCK;
use crate::device::{AnnouncedDevice, Device, DeviceKey, Parent, Relation, Role};
use crate::sysfs::{Attribute, StaticAttribute};

pub fn string_prop<'a>(properties: &'a mbus::Properties, name: &str) -> Option<&'a str> {
    match properties.get(name) {
        Some(mbus::Item::String(value)) => Some(value),
        _ => None,
    }
}

pub fn required_prop<'a>(properties: &'a mbus::Properties, name: &str) -> Result<&'a str> {
    string_prop(properties, name).with_context(|| format!("entity lacks the {name} property"))
}

pub fn parse_prop<T: FromStr>(properties: &mbus::Properties, name: &str) -> Result<T>
where
    T::Err: std::fmt::Display,
{
    let value = required_prop(properties, name)?;
    value
        .parse()
        .map_err(|e| anyhow!("failed to parse the {name} property '{value}': {e}"))
}

pub fn hex_prop(properties: &mbus::Properties, name: &str) -> Result<u32> {
    let value = required_prop(properties, name)?;
    u32::from_str_radix(value, 16)
        .with_context(|| format!("failed to parse the {name} property '{value}'"))
}

/// The entity's `drvcore.mbus-parent`; absent or -1 means that it has no parent.
pub fn mbus_parent(properties: &mbus::Properties) -> Option<i64> {
    let id: i64 = string_prop(properties, "drvcore.mbus-parent")?
        .parse()
        .ok()?;
    (id >= 0).then_some(id)
}

/// The parent device of an entity.
pub fn parent_ref(properties: &mbus::Properties) -> Parent {
    match mbus_parent(properties) {
        Some(id) => Parent::Key(DeviceKey::primary(id)),
        None => Parent::None,
    }
}

/// Parses a `usb.parent-interface` value ("<configuration>.<interface>") into
/// the key of that interface of the USB device with the given mbus ID.
pub fn interface_key(mbus_id: i64, value: &str) -> Option<DeviceKey> {
    let (config, number) = value.split_once('.')?;
    Some(DeviceKey::new(
        mbus_id,
        Role::UsbInterface {
            config: config.parse().ok()?,
            number: number.parse().ok()?,
        },
    ))
}

/// Like [`parent_ref`], but an entity below a USB interface (`usb.parent-interface`)
/// is parented to that interface rather than to the USB device.
pub fn device_parent_ref(properties: &mbus::Properties) -> Parent {
    let Some(parent) = mbus_parent(properties) else {
        return Parent::None;
    };
    if let Some(interface) = string_prop(properties, "usb.parent-interface") {
        if let Some(key) = interface_key(parent, interface) {
            return Parent::Key(key);
        }
    }
    Parent::Key(DeviceKey::primary(parent))
}

pub async fn remote_lane(event: &mbus::EnumerationEvent) -> Result<hel::Handle> {
    event.entity().get_remote_lane().await.map_err(|e| {
        anyhow!(
            "failed to obtain the lane of entity {}: {e}",
            event.entity_id()
        )
    })
}

/// A static text attribute.
pub fn attr(name: &str, contents: impl Into<Vec<u8>>) -> (String, Arc<dyn Attribute>) {
    (name.to_string(), StaticAttribute::new(contents))
}

/// What an [`Observation`] can own on behalf of an entity.
pub trait Installed: Send + 'static {
    /// The device that the item owns, if it owns one.
    fn device(&self) -> Option<&Device> {
        None
    }
}

impl Installed for AnnouncedDevice {
    fn device(&self) -> Option<&Device> {
        Some(self)
    }
}

impl Installed for Relation {}

/// What the installation of each entity left behind, by mbus ID.
type Slots = Arc<Mutex<BTreeMap<i64, Vec<Box<dyn Installed>>>>>;

/// An mbus enumeration loop together with everything that it installed.
struct Observation {
    slots: Slots,
}

impl Observation {
    fn new() -> Self {
        Self {
            slots: Arc::new(Mutex::new(BTreeMap::new())),
        }
    }

    async fn run(
        &self,
        filter: mbus::Filter<'_, '_, '_>,
        mut on_created: impl FnMut(Scope, mbus::EnumerationEvent),
    ) -> Result<()> {
        let mut enumerator = mbus::Enumerator::new(filter);
        loop {
            let (_, events) = enumerator
                .next_events()
                .await
                .map_err(|e| anyhow!("mbus enumeration failed: {e}"))?;
            for event in events {
                // TODO: The mbus enumerator does not report removals yet. A removal
                //       should drop what the installation of the entity left behind.
                if event.event_type() != mbus::EventType::Created {
                    continue;
                }
                let scope = Scope {
                    entity: event.entity_id(),
                    slots: self.slots.clone(),
                };
                on_created(scope, event);
            }
        }
    }
}

/// Runs an enumeration loop, calling `on_created` for every newly created
/// entity. What `on_created` installs through its [`Scope`] belongs to the
/// loop.
pub async fn observe(
    filter: mbus::Filter<'_, '_, '_>,
    on_created: impl FnMut(Scope, mbus::EnumerationEvent),
) -> Result<()> {
    Observation::new().run(filter, on_created).await
}

/// Installs the entity of one enumeration event into the [`Observation`]
/// that reported it.
pub struct Scope {
    entity: i64,
    slots: Slots,
}

impl Scope {
    /// Runs the installation of the entity on a detached task, so that an
    /// entity that waits for its parent does not stall the loop. What the
    /// installation returns is kept by the observation. A failure is logged
    /// and affects nothing else.
    // TODO: Dropping what an installation created should remove the devices;
    //       devices cannot be removed yet.
    pub fn install(
        self,
        what: String,
        task: impl Future<Output = Result<Vec<Box<dyn Installed>>>> + 'static,
    ) {
        let Scope { entity, slots } = self;
        hel::spawn(async move {
            match task
                .await
                .and_then(|installed| check_keys(entity, installed))
            {
                Ok(installed) => {
                    slots.lock().expect(EXPECT_LOCK).insert(entity, installed);
                }
                Err(e) => eprintln!("devserver: failed to install {what}: {e:#}"),
            }
        });
    }
}

// The registry promises that a key names a device of that entity.
fn check_keys(entity: i64, installed: Vec<Box<dyn Installed>>) -> Result<Vec<Box<dyn Installed>>> {
    for device in installed.iter().filter_map(|item| item.device()) {
        match device.key() {
            Some(key) if key.mbus_id == entity => {}
            key => bail!(
                "{} is keyed with {key:?} instead of the mbus ID {entity} of its entity",
                device.dir().sysfs_path()
            ),
        }
    }
    Ok(installed)
}
