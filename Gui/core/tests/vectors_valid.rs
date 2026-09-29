use serde_json::Value;

#[test]
fn test_vectors_json_is_well_formed() {
    let raw = std::fs::read_to_string(
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../protocol/test_vectors.json"),
    )
    .expect("test_vectors.json 可读");
    let v: Value = serde_json::from_str(&raw).expect("合法 JSON");
    assert_eq!(v["crc_sanity"]["crc"], "29B1");
    let frames = v["frames"].as_array().expect("frames 数组");
    assert_eq!(frames.len(), 4);
    for f in frames {
        let hex = f["hex"].as_str().unwrap();
        assert!(hex.len() >= 18 && hex.len() % 2 == 0, "hex 长度合法");
        let payload_hex = f["expect"]["payload_hex"].as_str().unwrap();
        let frame_bytes = hex.len() / 2;
        // 帧长 = 2(SOF)+5(hdr)+payload+2(CRC)
        assert_eq!(frame_bytes, 2 + 5 + payload_hex.len() / 2 + 2);
    }
}
