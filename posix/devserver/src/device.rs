//! Core device model and device state tracking
//!
//! Device construction happens in three stages:
//! - Subsystems first create a [`DeviceSpec`] that describes the immutable parts of the device.
//! - The device is created in sysfs via [`Model::create()`].
//!   This waits for the parent device to be created first.
//!   [`Model::create()`] returns an [`UnannouncedDevice`].
//! - The device's device node is created and its uevent is triggered via [`Model::announce()`].
//!   This waits for the parent device to be announced as well.
//!   [`Model::announce()`] turns the [`UnannouncedDevice`] into an [`AnnouncedDevice`].
//!   Change uevents are emitted through [`AnnouncedDevice::downgrade()`].

use std::any::Any;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::ops::Deref;
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex, Weak};

use anyhow::{Context, Result, bail};
use async_trait::async_trait;
use bragi::Message;
use event_listener::Event;
use hel::Handle;
use managarm::fs;

use crate::EXPECT_LOCK;
use crate::proto;
use crate::sysfs::{Attribute, SysfsNode};

/// ID allocator, for example for dri/card<N> numbers.
pub struct IdAllocator {
    next: u32,
    free: BTreeSet<u32>,
}

impl IdAllocator {
    pub fn new(first: u32) -> Self {
        Self {
            next: first,
            free: BTreeSet::new(),
        }
    }

    pub fn allocate(&mut self) -> u32 {
        if let Some(id) = self.free.pop_first() {
            return id;
        }
        let id = self.next;
        self.next += 1;
        id
    }

    #[allow(dead_code)]
    pub fn free(&mut self, id: u32) {
        self.free.insert(id);
    }
}

/// Unique identifier for a device in our model.
/// It refers to the device by mbus ID such that it can be constructed before the sysfs device path is known.
/// This is useful when a device in one subsystem wants to declare a parent in another subsystem.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct DeviceKey {
    pub mbus_id: i64,
    pub role: Role,
}

impl DeviceKey {
    pub fn new(mbus_id: i64, role: Role) -> Self {
        Self { mbus_id, role }
    }

    pub fn primary(mbus_id: i64) -> Self {
        Self::new(mbus_id, Role::Primary)
    }
}

/// Distinguishes the devices that share an mbus ID.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Role {
    /// Either the unique device corresponding to the mbus ID,
    /// or the one that we consider to be the primary one:
    /// - For DRM devices, the card<N> node.
    /// - For input devices, the input<N> node.
    /// - For sound devices, the card<N> node.
    Primary,
    /// The render<N> node of a DRM device.
    DrmRender,
    /// The event<N> evdev node of an input device.
    InputEvent,
    /// The controlC<N> node of a sound card.
    SoundControl,
    /// USB interface node with a specific (configuration value, interface number).
    UsbInterface { config: u8, number: u8 },
}

/// The parent of a device (either by [`DeviceKey`] or by direct pointer).
pub enum Parent {
    None,
    Key(DeviceKey),
    Device(Arc<Device>),
}

/// Location of the device in sysfs.
pub enum Placement {
    /// Lets the device model determine the placement based on the sysfs bus/class rules.
    Default,
    /// Specifies an explicit exception, for example to place devices in /sys/firmware.
    Under(Arc<SysfsNode>),
}

/// The sysfs subsystem of a device.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum Membership {
    None,
    /// Device belongs to the `/sys/bus/<subsystem>` hierarchy.
    Bus(String),
    /// Device belongs to the given class.
    Class(String),
    /// Device belongs to the block class and is linked from `/sys/block`.
    Block,
}

impl Membership {
    pub fn bus(name: &str) -> Self {
        Membership::Bus(name.to_string())
    }

    pub fn class(name: &str) -> Self {
        Membership::Class(name.to_string())
    }

    fn class_name(&self) -> Option<&str> {
        match self {
            Membership::Class(name) => Some(name),
            Membership::Block => Some("block"),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum NodeType {
    Char,
    Block,
}

/// Data needed to construct a /dev node.
/// Note that /dev nodes are managed by posix-subsystem but we tell posix-subsystem how to create them.
#[derive(Clone, Debug)]
pub struct DevNode {
    pub path: String,
    pub node_type: NodeType,
    pub major: i32,
    pub minor: i32,
}

/// Specifies the device node of a device:
/// either a device managed by us that has a lane or a foreign device without a lane.
pub enum DevNodeSpec {
    /// Device is enumerated by us and we have its lane.
    /// The lane is handled to posix-subsystem such that it can open the device.
    Managed(DevNode, Arc<Handle>),
    /// Device is not enumerated by us but built into posix-system.
    /// We do not have (or need) a lane, but we still need to represent the device in sysfs.
    Foreign(DevNode),
}

impl DevNodeSpec {
    pub fn node(&self) -> &DevNode {
        match self {
            DevNodeSpec::Managed(node, _) | DevNodeSpec::Foreign(node) => node,
        }
    }
}

/// Group of sysfs attributs that are created in a common subdirectory of a device.
/// This corresponds to Linux's attribute groups.
pub struct AttrGroup {
    name: String,
    attrs: Vec<(String, Arc<dyn Attribute>)>,
}

impl AttrGroup {
    pub fn named(name: impl Into<String>) -> Self {
        Self {
            name: name.into(),
            attrs: Vec::new(),
        }
    }

    pub fn attr(mut self, name: &str, attr: Arc<dyn Attribute>) -> Self {
        self.attrs.push((name.to_string(), attr));
        self
    }

    pub fn attrs(mut self, attrs: impl IntoIterator<Item = (String, Arc<dyn Attribute>)>) -> Self {
        self.attrs.extend(attrs);
        self
    }
}

/// Function to compose uevent key/values at runtime.
pub type UeventComposer = Box<dyn Fn(&mut BTreeMap<String, String>) + Send + Sync>;

/// The device data that are immutable after device creation.
pub struct DeviceSpec {
    name: String,
    parent: Parent,
    placement: Placement,
    membership: Membership,
    key: Option<DeviceKey>,
    devnode: Option<DevNodeSpec>,
    /// The DEVTYPE uevent value (e.g., `drm_minor`), if any.
    devtype: Option<String>,
    /// Subsystem-specific static uevent variables.
    uevent_extras: Vec<(String, String)>,
    /// Runtime-generated uevent variables.
    uevent_composer: Option<UeventComposer>,
    /// Attributes of the device directory itself.
    attrs: Vec<(String, Arc<dyn Attribute>)>,
    groups: Vec<AttrGroup>,
    data: Option<Arc<dyn Any + Send + Sync>>,
}

impl DeviceSpec {
    pub fn new(name: impl Into<String>, parent: Parent, membership: Membership) -> Self {
        Self {
            name: name.into(),
            parent,
            placement: Placement::Default,
            membership,
            key: None,
            devnode: None,
            devtype: None,
            uevent_extras: Vec::new(),
            uevent_composer: None,
            attrs: Vec::new(),
            groups: Vec::new(),
            data: None,
        }
    }

    pub fn attr(mut self, name: &str, attr: Arc<dyn Attribute>) -> Self {
        self.attrs.push((name.to_string(), attr));
        self
    }

    pub fn attrs(mut self, attrs: impl IntoIterator<Item = (String, Arc<dyn Attribute>)>) -> Self {
        self.attrs.extend(attrs);
        self
    }

    pub fn group(mut self, group: AttrGroup) -> Self {
        self.groups.push(group);
        self
    }

    pub fn uevent_composer(
        mut self,
        composer: impl Fn(&mut BTreeMap<String, String>) + Send + Sync + 'static,
    ) -> Self {
        self.uevent_composer = Some(Box::new(composer));
        self
    }

    pub fn key(mut self, key: DeviceKey) -> Self {
        self.key = Some(key);
        self
    }

    pub fn devtype(mut self, devtype: &str) -> Self {
        self.devtype = Some(devtype.to_string());
        self
    }

    pub fn devnode(mut self, devnode: DevNodeSpec) -> Self {
        self.devnode = Some(devnode);
        self
    }

    pub fn placement(mut self, placement: Placement) -> Self {
        self.placement = placement;
        self
    }

    pub fn uevent_extra(mut self, name: &str, value: impl Into<String>) -> Self {
        self.uevent_extras.push((name.to_string(), value.into()));
        self
    }

    /// Per-subsystem data that a subsystem needs to remember or pass to other subsystems.
    pub fn data(mut self, data: impl Any + Send + Sync) -> Self {
        self.data = Some(Arc::new(data));
        self
    }
}

/// State of a subsystem (i.e., a bus or class).
pub struct Subsystem {
    /// Sysfs directory of the subsystem.
    pub dir: Arc<SysfsNode>,
    /// devices/ subdirectory.
    pub devices_dir: Arc<SysfsNode>,
    /// drivers/ subdirectory (only for buses).
    pub drivers_dir: Option<Arc<SysfsNode>>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Lifecycle {
    /// Device is visible in sysfs but no uevent has been emitted yet.
    Created,
    /// announce() waits for the parent or installs the device node.
    Announcing,
    /// The device node exists and announce() is emitting the add uevent.
    Emitting,
    /// Device is visible in sysfs, its device node exists and the add uevent has been emitted.
    Announced,
}

/// State of a device. This is created from [`DeviceSpec`] but stores the live state.
pub struct Device {
    model: Weak<Model>,
    dir: Arc<SysfsNode>,
    name: String,
    parent: Option<Arc<Device>>,
    membership: Membership,
    devnode: Option<DevNodeSpec>,
    key: Option<DeviceKey>,
    devtype: Option<String>,
    uevent_extras: Vec<(String, String)>,
    uevent_composer: Option<UeventComposer>,
    data: Option<Arc<dyn Any + Send + Sync>>,
    lifecycle: Mutex<Lifecycle>,
    driver: Mutex<Option<String>>,
}

impl Device {
    pub fn dir(&self) -> &Arc<SysfsNode> {
        &self.dir
    }

    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn parent(&self) -> Option<&Arc<Device>> {
        self.parent.as_ref()
    }

    pub fn devnode(&self) -> Option<&DevNode> {
        self.devnode.as_ref().map(DevNodeSpec::node)
    }

    /// The subsystem data that the spec carried, if it is a `T`.
    pub fn data<T: Any + Send + Sync>(&self) -> Option<Arc<T>> {
        self.data.clone()?.downcast().ok()
    }

    fn is_class_device(&self) -> bool {
        self.membership.class_name().is_some()
    }

    fn lifecycle(&self) -> Lifecycle {
        *self.lifecycle.lock().expect(EXPECT_LOCK)
    }

    fn set_lifecycle(&self, lifecycle: Lifecycle) {
        *self.lifecycle.lock().expect(EXPECT_LOCK) = lifecycle;
    }

    pub fn key(&self) -> Option<&DeviceKey> {
        self.key.as_ref()
    }

    /// The name of the subsystem. This is reported by the SUBSYSTEM uevent key.
    pub fn subsystem_name(&self) -> Option<&str> {
        match &self.membership {
            Membership::None => None,
            Membership::Bus(name) | Membership::Class(name) => Some(name.as_str()),
            Membership::Block => Some("block"),
        }
    }

    /// Computes the uevent key/value pairs that are visible in the `uevent` sysfs file.
    /// Netlink uevents have extra key/value pairs that this function does not generate,
    /// the netlink-only key/value pairs are added in emit_uevent_with().
    pub fn compose_uevent(&self) -> BTreeMap<String, String> {
        let mut env = BTreeMap::new();
        if let Some(node) = self.devnode() {
            env.insert("DEVNAME".to_string(), node.path.clone());
            env.insert("MAJOR".to_string(), node.major.to_string());
            env.insert("MINOR".to_string(), node.minor.to_string());
        }
        // TODO: We are missing DEVMODE/DEVUID/DEVGID.
        if let Some(devtype) = &self.devtype {
            env.insert("DEVTYPE".to_string(), devtype.clone());
        }
        if let Some(key) = &self.key {
            env.insert("MBUS_ID".to_string(), key.mbus_id.to_string());
        }
        for (name, value) in &self.uevent_extras {
            env.insert(name.clone(), value.clone());
        }
        if let Some(composer) = &self.uevent_composer {
            composer(&mut env);
        }
        env
    }
}

/// Reference to a device that is not yet announced.
pub struct UnannouncedDevice {
    device: Arc<Device>,
}

impl Deref for UnannouncedDevice {
    type Target = Device;

    fn deref(&self) -> &Device {
        &self.device
    }
}

impl UnannouncedDevice {
    pub fn device(&self) -> &Arc<Device> {
        &self.device
    }
}

/// Reference to a device that is already announced.
pub struct AnnouncedDevice {
    device: Arc<Device>,
}

impl Deref for AnnouncedDevice {
    type Target = Device;

    fn deref(&self) -> &Device {
        &self.device
    }
}

impl AnnouncedDevice {
    pub fn device(&self) -> &Arc<Device> {
        &self.device
    }

    pub fn downgrade(&self) -> WeakAnnouncedDevice {
        WeakAnnouncedDevice {
            device: Arc::downgrade(&self.device),
        }
    }

    /// Leaks the device for the lifetime of the server.
    // TODO: It may be better to store these devices in a map in the Model.
    pub fn persist(self) {
        std::mem::forget(self);
    }
}

/// Weak reference to an announced device.
/// It can emit change uevents but it does not keep the device alive.
pub struct WeakAnnouncedDevice {
    device: Weak<Device>,
}

impl WeakAnnouncedDevice {
    /// Emits a change uevent. Returns false if the device is gone.
    pub async fn change(&self, extra: &[(String, String)]) -> Result<bool> {
        let Some(device) = self.device.upgrade() else {
            return Ok(false);
        };
        let model = device.model.upgrade().context("the device model is gone")?;
        model.emit_uevent_with("change", &device, extra).await?;
        Ok(true)
    }
}

/// Links between two devices.
/// For example, the `physical_node` link from ACPI devices to bus devices.
pub struct Relation;

struct UeventAttribute {
    device: Weak<Device>,
}

#[async_trait(?Send)]
impl Attribute for UeventAttribute {
    fn writable(&self) -> bool {
        true
    }

    async fn show(&self) -> Result<Vec<u8>, fs::server::Error> {
        let device = self
            .device
            .upgrade()
            .ok_or(fs::server::Error::InternalError)?;
        let mut out = String::new();
        for (name, value) in device.compose_uevent() {
            out += &format!("{name}={value}\n");
        }
        Ok(out.into_bytes())
    }

    // Writing an action emits a synthetic uevent (e.g., used by udevadm trigger).
    async fn store(&self, data: &[u8]) -> Result<(), fs::server::Error> {
        let device = self
            .device
            .upgrade()
            .ok_or(fs::server::Error::InternalError)?;
        let model = device
            .model
            .upgrade()
            .ok_or(fs::server::Error::InternalError)?;

        // Suppress synthetic uevents until the device is (being) announced:
        // before that, its device node may not exist yet and the real add uevent is still to come.
        // Since we allow synthetic uevents while the add uevent is emitted,
        // consumers may see both the real and the synthetic add uevent.
        // Linux has a similar mechanism (i.e., uevent_suppress()) but only some devices opt into it
        // (e.g., disks, partitions, ACPI devices, ttys and netdevs).
        // We are more general and suppress synthetic uevents for all devices.
        if matches!(
            device.lifecycle(),
            Lifecycle::Created | Lifecycle::Announcing
        ) {
            return Ok(());
        }

        let text = String::from_utf8_lossy(data);
        let action = match text.split_whitespace().next() {
            Some(action @ ("add" | "change")) => action,
            _ => return Ok(()),
        };
        model
            .emit_uevent(action, &device)
            .await
            .map_err(|_| fs::server::Error::InternalError)?;
        Ok(())
    }
}

// Root struct of the device model.
// Owns the device registry and the sysfs tree.
pub struct Model {
    events_lane: Handle,
    pub root: Arc<SysfsNode>,
    pub devices_dir: Arc<SysfsNode>,
    pub virtual_dir: Arc<SysfsNode>,
    pub firmware_dir: Arc<SysfsNode>,
    class_root: Arc<SysfsNode>,
    bus_root: Arc<SysfsNode>,
    dev_char_dir: Arc<SysfsNode>,
    dev_block_dir: Arc<SysfsNode>,
    block_root: Arc<SysfsNode>,
    self_ref: Weak<Model>,

    // Serializes uevents.
    uevent_seqnum: async_lock::Mutex<u64>,

    /// Maps memberships to subsystem state.
    subsystems: Mutex<HashMap<Membership, Arc<Subsystem>>>,
    /// Maps `DeviceKey`s to their device state.
    registry: Mutex<HashMap<DeviceKey, Arc<Device>>>,
    /// Raised when devices are registered or change their lifecycle state.
    pub update: Event,
}

impl Model {
    pub fn new(events_lane: Handle) -> Arc<Self> {
        let root = SysfsNode::new_root();
        let devices_dir = root.dir("devices").unwrap();
        let virtual_dir = devices_dir.dir("virtual").unwrap();
        let class_root = root.dir("class").unwrap();
        let bus_root = root.dir("bus").unwrap();
        let block_root = root.dir("block").unwrap();
        let dev_dir = root.dir("dev").unwrap();
        let dev_char_dir = dev_dir.dir("char").unwrap();
        let dev_block_dir = dev_dir.dir("block").unwrap();
        let firmware_dir = root.dir("firmware").unwrap();
        // cgroupfs is implemented by posix-subsystem, we just create the directory.
        root.dir("fs").unwrap().dir("cgroup").unwrap();

        Arc::new_cyclic(|self_ref| Self {
            events_lane,
            root,
            devices_dir,
            virtual_dir,
            firmware_dir,
            class_root,
            bus_root,
            dev_char_dir,
            dev_block_dir,
            block_root,
            self_ref: self_ref.clone(),
            uevent_seqnum: async_lock::Mutex::new(0u64),
            subsystems: Mutex::new(HashMap::new()),
            registry: Mutex::new(HashMap::new()),
            update: Event::new(),
        })
    }

    /// Get or create the subsystem state.
    /// The subsystem directories are created on first use.
    pub fn subsystem(&self, membership: &Membership) -> Result<Option<Arc<Subsystem>>> {
        let name = match membership {
            Membership::None => return Ok(None),
            Membership::Bus(name) | Membership::Class(name) => name.as_str(),
            Membership::Block => "block",
        };
        let mut subsystems = self.subsystems.lock().expect(EXPECT_LOCK);
        if let Some(existing) = subsystems.get(membership) {
            return Ok(Some(existing.clone()));
        }
        let subsystem = Arc::new(if matches!(membership, Membership::Bus(_)) {
            let dir = self.bus_root.dir(name)?;
            Subsystem {
                devices_dir: dir.dir("devices")?,
                drivers_dir: Some(dir.dir("drivers")?),
                dir,
            }
        } else {
            let dir = self.class_root.dir(name)?;
            Subsystem {
                devices_dir: dir.clone(),
                dir,
                drivers_dir: None,
            }
        });
        subsystems.insert(membership.clone(), subsystem.clone());
        Ok(Some(subsystem))
    }

    /// Retrieves a device if it exists.
    pub fn lookup(&self, key: &DeviceKey) -> Option<Arc<Device>> {
        self.registry.lock().expect(EXPECT_LOCK).get(key).cloned()
    }

    /// Waits for a device to be registered.
    /// This is used to wait for the parents of devices before creating them.
    pub async fn wait_device(&self, key: &DeviceKey) -> Arc<Device> {
        let mut logged = false;
        loop {
            let listener = self.update.listen();
            if let Some(device) = self.lookup(key) {
                if logged {
                    println!("devserver: {key:?} appeared as {}", device.dir.sysfs_path());
                }
                return device;
            }
            if !logged {
                println!("devserver: {key:?} is not available yet");
                logged = true;
            }
            listener.await;
        }
    }

    /// Wait for `Model::update` until the givne predicate returns true.
    pub async fn wait_until(&self, mut done: impl FnMut() -> bool) {
        loop {
            let listener = self.update.listen();
            if done() {
                return;
            }
            listener.await;
        }
    }

    pub async fn resolve_parent(&self, parent: Parent) -> Option<Arc<Device>> {
        match parent {
            Parent::None => None,
            Parent::Key(key) => Some(self.wait_device(&key).await),
            Parent::Device(device) => Some(device),
        }
    }

    /// Creates the device in sysfs.
    pub async fn create(&self, spec: DeviceSpec) -> Result<UnannouncedDevice> {
        let parent = self.resolve_parent(spec.parent).await;
        let subsystem = self.subsystem(&spec.membership)?;

        let dir_parent = match spec.placement {
            Placement::Under(dir) => dir,
            Placement::Default => match (spec.membership.class_name(), &parent) {
                // Emulate Linux's class device handling:
                // If a class device is nested inside another class, its directory sits directly in the parent directory.
                // Otherwise, a glue directory with the class's name is created.
                (Some(name), Some(parent)) => {
                    if parent.is_class_device() {
                        parent.dir.clone()
                    } else {
                        parent.dir.dir(name)?
                    }
                }
                (Some(name), None) => self.virtual_dir.dir(name)?,
                (_, Some(parent)) => parent.dir.clone(),
                (_, None) => self.devices_dir.clone(),
            },
        };
        let dir = dir_parent.create_dir(&spec.name)?;

        let device = UnannouncedDevice {
            device: Arc::new(Device {
                model: self.self_ref.clone(),
                dir: dir.clone(),
                name: spec.name.clone(),
                parent: parent.clone(),
                membership: spec.membership.clone(),
                devnode: spec.devnode,
                key: spec.key,
                devtype: spec.devtype,
                uevent_extras: spec.uevent_extras,
                uevent_composer: spec.uevent_composer,
                data: spec.data,
                lifecycle: Mutex::new(Lifecycle::Created),
                driver: Mutex::new(None),
            }),
        };
        // After register(), other devices can find the new device as their parent device.
        if let Some(key) = &device.key {
            self.register(key, device.device())?;
        }

        dir.create_attr(
            "uevent",
            Arc::new(UeventAttribute {
                device: Arc::downgrade(device.device()),
            }),
        )?;
        for (name, attr) in spec.attrs {
            dir.create_attr(&name, attr)?;
        }
        for group in spec.groups {
            let group_dir = dir.create_dir(&group.name)?;
            for (name, attr) in group.attrs {
                group_dir.create_attr(&name, attr)?;
            }
        }

        if let Some(subsystem) = &subsystem {
            dir.create_link("subsystem", &subsystem.dir)?;
            subsystem.devices_dir.create_link(&spec.name, &dir)?;
            if spec.membership == Membership::Block {
                self.block_root.create_link(&spec.name, &dir)?;
            }
            // Class devices have a parent symlink.
            if matches!(spec.membership, Membership::Class(_) | Membership::Block) {
                if let Some(parent) = &parent {
                    dir.create_link("device", &parent.dir)?;
                }
            }
        }

        // We do not create /sys/dev/{char,block} for devices without a proper subsystem
        // (i.e., the ones creates by the generic subsystem in generic.rs).
        // Otherwise, these symlinks would collide with subsystems that add more specific children (see usbmisc.rs).
        // TODO: This is a quirk of Managarm's mbus -> sysfs translation.
        //       We should consider removing the generic subsystem,
        //       or at least we should stop using it for non-leaf devices.
        if let (Some(node), Some(_)) = (device.devnode(), &subsystem) {
            let registry = match node.node_type {
                NodeType::Char => &self.dev_char_dir,
                NodeType::Block => &self.dev_block_dir,
            };
            registry.create_link(&format!("{}:{}", node.major, node.minor), &dir)?;
        }

        Ok(device)
    }

    fn register(&self, key: &DeviceKey, device: &Arc<Device>) -> Result<()> {
        let mut registry = self.registry.lock().expect(EXPECT_LOCK);
        if let Some(existing) = registry.get(key) {
            bail!(
                "device key {key:?} is registered for {} and cannot be reused for {}",
                existing.dir.sysfs_path(),
                device.name
            );
        }
        registry.insert(key.clone(), device.clone());
        drop(registry);
        self.update.notify(usize::MAX);
        Ok(())
    }

    /// Requests POSIX to create the device node and emits the add uevent.
    pub async fn announce(&self, device: UnannouncedDevice) -> Result<AnnouncedDevice> {
        device.set_lifecycle(Lifecycle::Announcing);
        if let Some(parent) = &device.parent {
            self.wait_announced(&device.dir.sysfs_path(), parent).await;
        }
        if let Some(DevNodeSpec::Managed(node, lane)) = &device.devnode {
            self.install_node(node, lane).await?;
        }
        // Allow synthetic uevents before emitting the add uevent.
        // Otherwise, a synthetic uevent that races with the add uevent is suppressed,
        // although the add uevent may be broadcast before the listener that triggered it exists.
        device.set_lifecycle(Lifecycle::Emitting);
        self.emit_uevent("add", &device).await?;
        device.set_lifecycle(Lifecycle::Announced);
        self.update.notify(usize::MAX);
        Ok(AnnouncedDevice {
            device: device.device,
        })
    }

    /// Wait until a given device entered announced state.
    async fn wait_announced(&self, waiter: &str, device: &Device) {
        let mut logged = false;
        loop {
            let listener = self.update.listen();
            if device.lifecycle() == Lifecycle::Announced {
                if logged {
                    println!(
                        "devserver: {waiter} continues since {} is now announced",
                        device.dir.sysfs_path()
                    );
                }
                return;
            }
            if !logged {
                println!(
                    "devserver: {waiter} waits for {} to be announced",
                    device.dir.sysfs_path()
                );
                logged = true;
            }
            listener.await;
        }
    }

    pub async fn create_and_announce(&self, spec: DeviceSpec) -> Result<AnnouncedDevice> {
        let device = self.create(spec).await?;
        self.announce(device).await
    }

    /// Bind a bus driver to a device.
    pub async fn bind_driver(&self, device: &Device, driver: &str) -> Result<()> {
        let drivers_dir = self
            .subsystem(&device.membership)?
            .and_then(|subsystem| subsystem.drivers_dir.clone())
            .with_context(|| format!("{} is not a bus device", device.dir.sysfs_path()))?;
        let mut bound = device.driver.lock().expect(EXPECT_LOCK);
        if bound.is_some() {
            return Ok(());
        }
        let driver_dir = drivers_dir.dir(driver)?;
        driver_dir.create_link(&device.name, &device.dir)?;
        device.dir.create_link("driver", &driver_dir)?;
        *bound = Some(driver.to_string());
        Ok(())
    }

    /// Links `from` to `to` and back.
    /// The forward link is called `name`, the backwards link is called `back_name`.
    pub fn relate(
        &self,
        from: &Device,
        name: &str,
        to: &Device,
        back_name: Option<&str>,
    ) -> Result<Relation> {
        from.dir.create_link(name, &to.dir)?;
        if let Some(back_name) = back_name {
            to.dir.create_link(back_name, &from.dir)?;
        }
        Ok(Relation)
    }

    /// Tells posix-subsystem to create a device node.
    async fn install_node(&self, node: &DevNode, lane: &Handle) -> Result<()> {
        println!(
            "devserver: mknod /dev/{} ({}:{})",
            node.path, node.major, node.minor
        );
        let node_type = match node.node_type {
            NodeType::Char => proto::NodeType::CharDevice,
            NodeType::Block => proto::NodeType::BlockDevice,
        };
        let req =
            proto::InstallNodeRequest::new(node_type, node.major, node.minor, node.path.clone());
        self.events_request_with_lane(&req, lane).await
    }

    async fn emit_uevent(&self, action: &str, device: &Device) -> Result<()> {
        self.emit_uevent_with(action, device, &[]).await
    }

    /// Emits a uevent with additional key/value pairs.
    // TODO: For parity with Linux, we should suppress the uevent of a device without a subsystem.
    //       See dev_uevent_filter() in Linux.
    async fn emit_uevent_with(
        &self,
        action: &str,
        device: &Device,
        extra: &[(String, String)],
    ) -> Result<()> {
        let devpath = device.dir.sysfs_path();
        if crate::TRACE.load(Ordering::Relaxed) {
            println!("devserver: uevent {action}@/{devpath}");
        }
        let env = device.compose_uevent();
        // Take the seqnum lock until posix-subsystem has processed the uevent.
        let mut seqnum_guard = self.uevent_seqnum.lock().await;
        *seqnum_guard += 1;
        let seqnum = *seqnum_guard;

        // The order follows Linux' kobject_uevent_env().
        // Note that the extra key/value pairs come before the device's.
        let mut buffer = format!("{action}@/{devpath}\0");
        buffer += &format!("ACTION={action}\0");
        buffer += &format!("DEVPATH=/{devpath}\0");
        if let Some(subsystem) = device.subsystem_name() {
            buffer += &format!("SUBSYSTEM={subsystem}\0");
        }
        for (name, value) in extra {
            buffer += &format!("{name}={value}\0");
        }
        for (name, value) in &env {
            buffer += &format!("{name}={value}\0");
        }
        buffer += &format!("SEQNUM={seqnum}\0");

        let req = proto::EmitUeventRequest::new(buffer);
        self.events_request(&req).await
    }

    async fn events_request<M: Message>(&self, req: &M) -> Result<()> {
        let (head, tail) = bragi::head_tail_to_bytes(req)?;
        let (_offer, (send_head, send_tail, recv_resp)) = hel::submit_async(
            &self.events_lane,
            hel::Offer::new((
                hel::SendBuffer::new(&head),
                hel::SendBuffer::new(&tail),
                hel::ReceiveInline,
            )),
        )
        .await?;
        send_head?;
        send_tail?;
        let recv_resp = recv_resp?;

        let resp: proto::GenericResponse = bragi::head_from_bytes(&recv_resp)?;
        if resp.error() != proto::Errors::Success {
            bail!("posix failed a device events request: {:?}", resp.error());
        }
        Ok(())
    }

    async fn events_request_with_lane<M: Message>(&self, req: &M, lane: &Handle) -> Result<()> {
        let (head, tail) = bragi::head_tail_to_bytes(req)?;
        let (_offer, (send_head, send_tail, push_descriptor, recv_resp)) = hel::submit_async(
            &self.events_lane,
            hel::Offer::new((
                hel::SendBuffer::new(&head),
                hel::SendBuffer::new(&tail),
                hel::PushDescriptor::new(lane, hel_sys::kHelRightInvoke | hel_sys::kHelRightManage),
                hel::ReceiveInline,
            )),
        )
        .await?;
        send_head?;
        send_tail?;
        push_descriptor?;
        let recv_resp = recv_resp?;

        let resp: proto::GenericResponse = bragi::head_from_bytes(&recv_resp)?;
        if resp.error() != proto::Errors::Success {
            bail!("posix failed a device events request: {:?}", resp.error());
        }
        Ok(())
    }
}
