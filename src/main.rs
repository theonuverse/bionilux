use clap::Parser;
use std::env;
use std::process::exit;

mod analyzer;
mod debug;
mod launcher;

use launcher::config::RuntimeConfig;

#[derive(Parser, Debug)]
#[command(
    name = "bionilux",
    version,
    about = "Run glibc and x86_64 Linux binaries on Termux",
    disable_help_subcommand = true,
    arg_required_else_help = true,
    after_help = "Examples:\n  \
        bionilux ./my_binary                    # glibc or x86_64, auto-detected\n  \
        bionilux $PREFIX/glibc/bin/bash         # glibc shell\n  \
        BIONILUX_DEBUG=1 bionilux ./my_app      # verbose\n  \
        bionilux -n ./static_glibc_binary       # skip preload injection\n  \
        bionilux --print-config                 # show resolved runtime paths\n  \
        bionilux ls -la                         # pass args through"
)]
struct Cli {
    /// Print debug output to stderr
    #[arg(short, long)]
    debug: bool,

    /// Skip LD_PRELOAD injection (no child rerouting)
    #[arg(short = 'n', long = "no-preload")]
    no_preload: bool,

    /// Print resolved runtime paths and exit
    #[arg(long = "print-config")]
    print_config: bool,

    /// Target binary to run
    binary: Option<String>,

    /// Arguments passed to the target binary
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    args: Vec<String>,
}

fn main() {
    let cli = Cli::parse();
    debug::init(cli.debug);

    let config = RuntimeConfig::from_env();

    if cli.print_config {
        print!("{}", config.format());
        return;
    }

    let Some(binary) = cli.binary else {
        eprintln!("error: missing target binary");
        exit(2);
    };

    let mut all_args = vec![binary.clone()];
    all_args.extend(cli.args.iter().cloned());

    let mut execution = match launcher::resolve::resolve(&binary, &all_args, &config) {
        Ok(e) => e,
        Err(err) => {
            eprintln!("error: {err}");
            exit(err.exit_code());
        }
    };

    if cli.no_preload {
        execution.preload = None;
    }

    let chdir_to = if env::var_os("BIONILUX_CHDIR_TO_BINARY").is_some() {
        execution.original_target.parent()
    } else {
        None
    };

    let current_exe = env::current_exe().expect("cannot resolve current executable");
    let target_env = launcher::env::build_target_env(
        execution.preload.as_deref().and_then(|p| p.to_str()),
        &execution.exec_target,
        &current_exe,
        execution.is_x86_64,
        &config,
    );

    if let Err(e) = launcher::exec::execute(
        &execution.exec_target,
        execution.loader.as_deref(),
        &execution.argv,
        &target_env,
        chdir_to,
    ) {
        eprintln!("error: {e}");
        exit(1);
    }
}
