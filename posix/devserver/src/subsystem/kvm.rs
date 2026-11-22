//! The kvm device (/dev/kvm).

use std::sync::Arc;

use anyhow::Result;
use managarm::mbus;

use crate::device::{
    DevNode, DevNodeSpec, DeviceKey, DeviceSpec, Membership, Model, NodeType, Parent,
};
use crate::subsystem::{Installed, attr, observe, remote_lane};

const MISC_MAJOR: i32 = 10;
const KVM_MINOR: i32 = 232;

async fn install_entity(
    model: Arc<Model>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    println!("devserver: Installing KVM device");
    let lane = Arc::new(remote_lane(&event).await?);
    let spec = DeviceSpec::new("kvm".to_string(), Parent::None, Membership::class("misc"))
        .key(DeviceKey::primary(event.entity_id()))
        .devnode(DevNodeSpec::Managed(
            DevNode {
                path: "kvm".to_string(),
                node_type: NodeType::Char,
                major: MISC_MAJOR,
                minor: KVM_MINOR,
            },
            lane,
        ))
        .attrs(vec![attr("dev", format!("{MISC_MAJOR}:{KVM_MINOR}\n"))]);
    Ok(vec![Box::new(model.create_and_announce(spec).await?)])
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    observe(
        mbus::Filter::Equals("unix.subsystem", "kvm"),
        |scope, event| {
            scope.install(
                format!("KVM device {}", event.entity_id()),
                install_entity(model.clone(), event),
            );
        },
    )
    .await
}
