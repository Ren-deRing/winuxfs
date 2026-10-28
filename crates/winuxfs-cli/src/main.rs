// SPDX-License-Identifier: GPL-2.0-only
//! The `winuxfs` command.
//!
//! Exit codes: 0 success, 1 user input error, 2 mount failure, 3 missing
//! privilege. Errors are printed to stderr as one sentence saying what
//! failed and what to do about it.

use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};

mod cmd_list;
mod cmd_mount;
mod common;

use common::{CliError, EXIT_USAGE};

#[derive(Parser, Debug)]
#[command(
    name = "winuxfs",
    version,
    about = "Mount Linux file systems on Windows through WinFsp",
    after_help = winuxfs_win::WINFSP_NOTICE
)]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// List the backend plugins found and why broken ones failed to load.
    ListBackends {
        /// Plugin directory (repeatable); replaces the defaults, which are
        /// WINUXFS_PLUGIN_PATH and `plugins` next to the executable.
        #[arg(long = "plugin-dir", value_name = "DIR")]
        plugin_dir: Vec<PathBuf>,
    },
    /// Print a device's partition table (number, start and length in bytes,
    /// type).
    ListPartitions {
        /// Disk, VHD/VHDX or image file.
        device: String,
    },
    /// Mount a file system (Windows only); runs until Ctrl-C or `unmount`.
    Mount(cmd_mount::MountArgs),
    /// Unmount a file system mounted by `winuxfs mount` (Windows only).
    Unmount {
        /// The mount point given to `mount`.
        mountpoint: String,
    },
}

fn run(cli: Cli) -> Result<(), CliError> {
    match cli.cmd {
        Cmd::ListBackends { plugin_dir } => cmd_list::list_backends(&plugin_dir),
        Cmd::ListPartitions { device } => cmd_list::list_partitions(&device),
        Cmd::Mount(a) => cmd_mount::mount(&a),
        Cmd::Unmount { mountpoint } => cmd_mount::unmount(&mountpoint),
    }
}

fn main() -> ExitCode {
    let cli = match Cli::try_parse() {
        Ok(c) => c,
        Err(e) => {
            // clap uses exit code 2 for usage errors; ours is 1.
            let _ = e.print();
            return ExitCode::from(if e.use_stderr() { EXIT_USAGE as u8 } else { 0 });
        }
    };
    match run(cli) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("winuxfs: {}", e.msg);
            ExitCode::from(e.code as u8)
        }
    }
}
