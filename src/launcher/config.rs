use std::env;
use std::path::PathBuf;

const DEFAULT_PREFIX: &str = "/data/data/com.termux/files/usr";

// Resolved once at startup. All paths are derived from $PREFIX (or the
// Termux default) unless an explicit BIONILUX_* override is set.
pub struct RuntimeConfig {
    pub prefix: PathBuf,
    pub glibc_dir: PathBuf,
    pub loader: PathBuf,
    pub box64: PathBuf,
    pub termux_bin: PathBuf,
    pub ca_file: PathBuf,
    pub ca_dir: PathBuf,
}

impl RuntimeConfig {
    // Build a config with default derived paths under a given prefix.
    pub fn with_prefix(prefix: PathBuf) -> Self {
        Self {
            glibc_dir: prefix.join("glibc"),
            loader: prefix.join("glibc/lib/ld-linux-aarch64.so.1"),
            box64: prefix.join("bionilux/box64-glibc"),
            termux_bin: prefix.join("bin"),
            ca_file: prefix.join("etc/tls/cert.pem"),
            ca_dir: prefix.join("etc/tls/certs"),
            prefix,
        }
    }

    // Resolve from environment. Priority for the prefix itself is
    // BIONILUX_PREFIX > $PREFIX > Termux default. Individual paths can
    // be overridden by their own BIONILUX_* variable.
    pub fn from_env() -> Self {
        let prefix = resolve_prefix();
        let mut cfg = Self::with_prefix(prefix);
        if let Some(p) = env_path("BIONILUX_GLIBC_DIR") {
            cfg.glibc_dir = p;
        }
        if let Some(p) = env_path("BIONILUX_LOADER") {
            cfg.loader = p;
        }
        if let Some(p) = env_path("BIONILUX_BOX64") {
            cfg.box64 = p;
        }
        if let Some(p) = env_path("BIONILUX_CA_FILE") {
            cfg.ca_file = p;
        }
        if let Some(p) = env_path("BIONILUX_CA_DIR") {
            cfg.ca_dir = p;
        }
        cfg
    }

    // Ubuntu PT_INTERP says /lib/...; Termux stores it under $GLIBC/lib.
    pub fn find_loader(&self, interp: &str) -> Option<PathBuf> {
        let p = PathBuf::from(interp);
        if p.exists() {
            return Some(p);
        }
        let name = p.file_name()?;
        let alt = self.glibc_dir.join("lib").join(name);
        alt.exists().then_some(alt)
    }

    // Search for the preload library: env override, then near the current
    // executable (dev workflow), then installed alongside glibc.
    pub fn find_preload(&self) -> Option<PathBuf> {
        if let Ok(p) = env::var("BIONILUX_PRELOAD") {
            let p = PathBuf::from(p);
            if p.exists() {
                return Some(p);
            }
        }
        if let Ok(exe) = env::current_exe() {
            let mut dir = exe.parent();
            for _ in 0..3 {
                let Some(d) = dir else { break };
                let p = d.join("libbionilux_preload.so");
                if p.exists() {
                    return Some(p);
                }
                dir = d.parent();
            }
        }
        let p = self.glibc_dir.join("lib/libbionilux_preload.so");
        p.exists().then_some(p)
    }

    // Debug output for --print-config.
    pub fn format(&self) -> String {
        format!(
            concat!(
                "prefix      {}\n",
                "glibc_dir   {}\n",
                "loader      {}\n",
                "box64       {}\n",
                "termux_bin  {}\n",
                "ca_file     {}\n",
                "ca_dir      {}\n",
            ),
            self.prefix.display(),
            self.glibc_dir.display(),
            self.loader.display(),
            self.box64.display(),
            self.termux_bin.display(),
            self.ca_file.display(),
            self.ca_dir.display(),
        )
    }
}

fn resolve_prefix() -> PathBuf {
    if let Some(p) = env_path("BIONILUX_PREFIX") {
        return p;
    }
    if let Ok(p) = env::var("PREFIX")
        && !p.is_empty()
    {
        return PathBuf::from(p);
    }
    PathBuf::from(DEFAULT_PREFIX)
}

fn env_path(name: &str) -> Option<PathBuf> {
    env::var(name)
        .ok()
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn with_prefix_derives_all_paths() {
        let c = RuntimeConfig::with_prefix(PathBuf::from("/x"));
        assert_eq!(c.glibc_dir, PathBuf::from("/x/glibc"));
        assert_eq!(
            c.loader,
            PathBuf::from("/x/glibc/lib/ld-linux-aarch64.so.1")
        );
        assert_eq!(c.box64, PathBuf::from("/x/bionilux/box64-glibc"));
        assert_eq!(c.termux_bin, PathBuf::from("/x/bin"));
        assert_eq!(c.ca_file, PathBuf::from("/x/etc/tls/cert.pem"));
        assert_eq!(c.ca_dir, PathBuf::from("/x/etc/tls/certs"));
    }

    #[test]
    fn find_loader_returns_none_for_missing() {
        let c = RuntimeConfig::with_prefix(PathBuf::from("/x"));
        assert!(c.find_loader("/nonexistent/ld-fake.so").is_none());
    }
}
