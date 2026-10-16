use nix::unistd::execve;
use std::ffi::CString;
use std::path::Path;

use crate::debug;

// Replace the current process with exec_target.
// With a loader: execve(loader, [loader, --argv0, argv[0], exec_target, args[1..]]).
// Without:       execve(exec_target, argv).
//
// Returns Err only if CString conversion fails (NUL byte, non-UTF-8 path).
// On successful execve the process is replaced and this never returns.
pub fn execute(
    exec_target: &Path,
    loader: Option<&Path>,
    argv: &[String],
    env: &[String],
    chdir_to: Option<&Path>,
) -> Result<(), String> {
    if let Some(dir) = chdir_to {
        let _ = std::env::set_current_dir(dir);
    }

    // The hook passes the caller's argv[0] via BIONILUX_ARGV0 so login
    // shells (-bash), exec -a, and multicall binaries see their real name.
    let argv0_owned = std::env::var("BIONILUX_ARGV0").ok();
    let argv0 = argv0_owned
        .as_deref()
        .or_else(|| argv.first().map(String::as_str))
        .unwrap_or("");

    let c_env: Vec<CString> = env
        .iter()
        .map(|e| cstring_arg(e, "environment entry"))
        .collect::<Result<_, _>>()?;
    let c_target = cstring_path(exec_target)?;

    let (exec_path, c_args): (CString, Vec<CString>) = match loader {
        Some(ld) => {
            let c_ld = cstring_path(ld)?;
            let mut v = Vec::with_capacity(argv.len() + 3);
            v.push(c_ld.clone());
            v.push(CString::new("--argv0").unwrap());
            v.push(cstring_arg(argv0, "argv[0]")?);
            v.push(c_target);
            for a in argv.iter().skip(1) {
                v.push(cstring_arg(a, "argument")?);
            }
            (c_ld, v)
        }
        None => {
            let mut v = Vec::with_capacity(argv.len());
            if !argv0.is_empty() {
                v.push(cstring_arg(argv0, "argv[0]")?);
            }
            for a in argv.iter().skip(1) {
                v.push(cstring_arg(a, "argument")?);
            }
            (c_target, v)
        }
    };

    debug!("execve: {}", exec_path.to_string_lossy());

    // execve only returns on failure.
    let e = execve(&exec_path, &c_args, &c_env).unwrap_err();
    let code = match e {
        nix::errno::Errno::ENOENT => 127,
        nix::errno::Errno::EACCES => 126,
        _ => 1,
    };
    eprintln!("[ERROR] execve failed: {e}");
    std::process::exit(code);
}

fn cstring_arg(s: &str, what: &str) -> Result<CString, String> {
    CString::new(s).map_err(|_| format!("{what} contains a NUL byte"))
}

fn cstring_path(p: &Path) -> Result<CString, String> {
    let s = p
        .to_str()
        .ok_or_else(|| format!("non-UTF-8 path: {}", p.display()))?;
    CString::new(s).map_err(|_| format!("path contains a NUL byte: {}", p.display()))
}
