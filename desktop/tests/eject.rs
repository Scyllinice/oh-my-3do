#![cfg(target_os = "linux")]
use oh_my_3do_library::eject::{linux_remove, linux_target, LinuxTarget};
use serde_json::json;
use std::path::Path;
fn listing() -> serde_json::Value {
    json!({"blockdevices":[{"path":"/dev/sdz","type":"disk","rm":true,"hotplug":true,"mountpoints":[null],"children":[{"path":"/dev/sdz1","type":"part","mountpoints":["/media/card"]}]}]})
}
#[test]
fn only_exact_removable_mount_is_accepted() {
    let data = listing();
    assert_eq!(
        linux_target(Path::new("/media/card"), &data).unwrap(),
        LinuxTarget {
            device: "/dev/sdz1".into(),
            disk: "/dev/sdz".into()
        }
    );
    for root in ["/", "/media/card/Games", "/mnt/k"] {
        assert!(linux_target(Path::new(root), &data).is_err());
    }
    let mut fixed = data.clone();
    fixed["blockdevices"][0]["rm"] = json!(false);
    fixed["blockdevices"][0]["hotplug"] = json!(false);
    assert!(linux_target(Path::new("/media/card"), &fixed).is_err());
    let mut other = data.clone();
    other["blockdevices"][0]["children"]
        .as_array_mut()
        .unwrap()
        .push(json!({"type":"part","path":"/dev/sdz2","mountpoints":["/media/other"]}));
    assert!(linux_target(Path::new("/media/card"), &other).is_err());
}
#[test]
fn removal_never_powers_off_after_failed_unmount() {
    let target = linux_target(Path::new("/media/card"), &listing()).unwrap();
    let mut calls = vec![];
    assert!(linux_remove(&target, |args| {
        calls.push(args.join(" "));
        anyhow::bail!("busy")
    })
    .is_err());
    assert_eq!(calls.len(), 1);
    assert!(calls[0].starts_with("unmount "));
    assert!(!calls[0].contains("force"));
    calls.clear();
    linux_remove(&target, |args| {
        calls.push(args.join(" "));
        Ok(())
    })
    .unwrap();
    assert_eq!(
        calls,
        vec![
            "unmount --block-device /dev/sdz1 --no-user-interaction",
            "power-off --block-device /dev/sdz --no-user-interaction"
        ]
    );
    let failure = linux_remove(&target, |args| {
        if args[0] == "power-off" {
            anyhow::bail!("unsupported")
        }
        Ok(())
    })
    .unwrap_err();
    assert!(failure.to_string().contains("did not confirm"));
}
#[test]
fn status_offers_eject_only_for_a_connected_removable_root() {
    let folder = std::env::temp_dir().join(format!("oh3do-status-{}", std::process::id()));
    std::fs::create_dir_all(&folder).unwrap();
    let present = oh_my_3do_library::eject::status(&folder);
    assert!(present.connected && !present.removable);
    let missing = oh_my_3do_library::eject::status(&folder.join("missing"));
    assert!(!missing.connected && !missing.removable);
    assert!(!oh_my_3do_library::eject::status(Path::new("/")).removable);
    std::fs::remove_dir_all(&folder).unwrap();
}
