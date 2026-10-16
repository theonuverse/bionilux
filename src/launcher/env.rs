use std::collections::HashMap;
use std::env;
use std::path::Path;

use crate::launcher::config::RuntimeConfig;

const BIONIC_PRELOAD_BLOCKLIST: &[&str] = &["libtermux-exec"];

fn filter_preload(entry_list: &str) -> Vec<String> {
    entry_list
        .split(':')
        .filter(|e| !e.is_empty())
        .filter(|e| !BIONIC_PRELOAD_BLOCKLIST.iter().any(|bad| e.contains(bad)))
        .map(String::from)
        .collect()
}

fn setup_tls(vars: &mut HashMap<String, String>, config: &RuntimeConfig) {
    if !vars.contains_key("SSL_CERT_FILE") && config.ca_file.exists() {
        vars.insert(
            "SSL_CERT_FILE".into(),
            config.ca_file.to_string_lossy().into_owned(),
        );
    }
    if !vars.contains_key("SSL_CERT_DIR") && config.ca_dir.exists() {
        vars.insert(
            "SSL_CERT_DIR".into(),
            config.ca_dir.to_string_lossy().into_owned(),
        );
    }
}

pub fn build_target_env(
    preload: Option<&str>,
    target: &Path,
    launcher: &Path,
    is_x86_64: bool,
    config: &RuntimeConfig,
) -> Vec<String> {
    let vars: HashMap<String, String> = env::vars().collect();
    build_from(vars, preload, target, launcher, is_x86_64, config)
}

fn build_from(
    mut vars: HashMap<String, String>,
    preload: Option<&str>,
    target: &Path,
    launcher: &Path,
    is_x86_64: bool,
    config: &RuntimeConfig,
) -> Vec<String> {
    vars.remove("BIONILUX_TARGET_EXE");
    vars.remove("BIONILUX_BINARY_PATH");
    vars.remove("BIONILUX_ARGV0");
    vars.remove("LD_AUDIT");
    vars.remove("LD_DEBUG");

    // Pass the glibc directory to the hook so it redirects /etc/*
    // consistently with the launcher, regardless of prefix.
    vars.insert(
        "BIONILUX_GLIBC_PREFIX".into(),
        config.glibc_dir.to_string_lossy().into_owned(),
    );

    match preload {
        Some(p) => {
            let existing = vars.get("LD_PRELOAD").cloned().unwrap_or_default();
            let mut merged = filter_preload(&existing);
            merged.insert(0, p.to_string());

            vars.insert("LD_PRELOAD".into(), merged.join(":"));
            vars.insert("BIONILUX_TARGET_EXE".into(), target.display().to_string());
            vars.insert(
                "BIONILUX_BINARY_PATH".into(),
                launcher.display().to_string(),
            );

            setup_tls(&mut vars, config);
        }
        None => {
            vars.remove("LD_PRELOAD");
        }
    }

    if is_x86_64 {
        vars.insert("BOX64_UNAME".into(), "x86_64".into());
    } else {
        vars.remove("BOX64_UNAME");
    }

    vars.into_iter().map(|(k, v)| format!("{k}={v}")).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn test_config() -> RuntimeConfig {
        RuntimeConfig::with_prefix(PathBuf::from("/test/prefix"))
    }

    // ── filter_preload ──────────────────────────────────────────────

    #[test]
    fn filter_drops_empty() {
        assert!(filter_preload("").is_empty());
    }

    #[test]
    fn filter_drops_termux_exec() {
        let r = filter_preload("/a/libtermux-exec.so:/b/keep.so");
        assert_eq!(r, vec!["/b/keep.so"]);
    }

    #[test]
    fn filter_keeps_normal_entries() {
        let r = filter_preload("/a/x.so:/b/y.so");
        assert_eq!(r, vec!["/a/x.so", "/b/y.so"]);
    }

    // ── build_from ──────────────────────────────────────────────────

    fn base_vars() -> HashMap<String, String> {
        let mut m = HashMap::new();
        m.insert("PATH".into(), "/usr/bin".into());
        m
    }

    #[test]
    fn build_sets_preload_and_marker_vars() {
        let c = test_config();
        let out = build_from(
            base_vars(),
            Some("/so/pre.so"),
            Path::new("/target"),
            Path::new("/launcher"),
            false,
            &c,
        );
        let joined = out.join("\n");
        assert!(joined.contains("LD_PRELOAD=/so/pre.so"));
        assert!(joined.contains("BIONILUX_TARGET_EXE=/target"));
        assert!(joined.contains("BIONILUX_BINARY_PATH=/launcher"));
        assert!(joined.contains("BIONILUX_GLIBC_PREFIX=/test/prefix/glibc"));
    }

    #[test]
    fn build_prepends_to_existing_preload() {
        let c = test_config();
        let mut vars = base_vars();
        vars.insert("LD_PRELOAD".into(), "/old/a.so".into());
        let out = build_from(
            vars,
            Some("/so/pre.so"),
            Path::new("/target"),
            Path::new("/launcher"),
            false,
            &c,
        );
        let joined = out.join("\n");
        assert!(joined.contains("LD_PRELOAD=/so/pre.so:/old/a.so"));
    }

    #[test]
    fn build_strips_termux_exec_from_merged() {
        let c = test_config();
        let mut vars = base_vars();
        vars.insert("LD_PRELOAD".into(), "/usr/lib/libtermux-exec.so".into());
        let out = build_from(
            vars,
            Some("/so/pre.so"),
            Path::new("/target"),
            Path::new("/launcher"),
            false,
            &c,
        );
        let joined = out.join("\n");
        assert!(joined.contains("LD_PRELOAD=/so/pre.so"));
        assert!(!joined.contains("libtermux-exec"));
    }

    #[test]
    fn build_no_preload_removes_ld_preload() {
        let c = test_config();
        let mut vars = base_vars();
        vars.insert("LD_PRELOAD".into(), "/old/a.so".into());
        let out = build_from(
            vars,
            None,
            Path::new("/target"),
            Path::new("/launcher"),
            false,
            &c,
        );
        let joined = out.join("\n");
        assert!(!joined.contains("LD_PRELOAD="));
    }

    #[test]
    fn build_sets_box64_uname_for_x86_64() {
        let c = test_config();
        let out = build_from(
            base_vars(),
            None,
            Path::new("/target"),
            Path::new("/launcher"),
            true,
            &c,
        );
        let joined = out.join("\n");
        assert!(joined.contains("BOX64_UNAME=x86_64"));
    }

    #[test]
    fn build_clears_box64_uname_for_native() {
        let c = test_config();
        let mut vars = base_vars();
        vars.insert("BOX64_UNAME".into(), "x86_64".into());
        let out = build_from(
            vars,
            None,
            Path::new("/target"),
            Path::new("/launcher"),
            false,
            &c,
        );
        let joined = out.join("\n");
        assert!(!joined.contains("BOX64_UNAME"));
    }

    #[test]
    fn build_removes_stale_bionilux_vars() {
        let c = test_config();
        let mut vars = base_vars();
        vars.insert("BIONILUX_TARGET_EXE".into(), "/stale".into());
        vars.insert("BIONILUX_BINARY_PATH".into(), "/stale".into());
        vars.insert("BIONILUX_ARGV0".into(), "/stale".into());
        let out = build_from(
            vars,
            None,
            Path::new("/target"),
            Path::new("/launcher"),
            false,
            &c,
        );
        let joined = out.join("\n");
        assert!(!joined.contains("/stale"));
    }
}
