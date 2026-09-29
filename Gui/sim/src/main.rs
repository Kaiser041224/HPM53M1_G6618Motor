//! gui-sim 可执行入口：`gui-sim [--port 9000] [--config ../config]`。

use gui_sim::{serve, SimDevice};
use std::sync::Arc;
use tokio::net::TcpListener;
use tokio::sync::Mutex;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut port: u16 = 9000;
    let mut config = String::from("../config");

    let args: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--port" => {
                if let Some(v) = args.get(i + 1) {
                    port = v.parse()?;
                    i += 2;
                } else {
                    i += 1;
                }
            }
            "--config" => {
                if let Some(v) = args.get(i + 1) {
                    config = v.clone();
                    i += 2;
                } else {
                    i += 1;
                }
            }
            _ => i += 1,
        }
    }

    let device = SimDevice::from_config_dir(std::path::Path::new(&config))?;
    let device = Arc::new(Mutex::new(device));

    let addr = format!("127.0.0.1:{port}");
    let listener = TcpListener::bind(&addr).await?;
    println!("gui-sim listening on {addr} (config: {config})");
    serve(listener, device).await?;
    Ok(())
}
