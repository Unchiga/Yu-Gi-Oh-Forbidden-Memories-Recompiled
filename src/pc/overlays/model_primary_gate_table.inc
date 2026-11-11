/* Written by tools/pc/model_gate.py from config/pc/model_primary_gate.csv: do not edit.
 * One row for each primary module the decomp's C stands in for:
 * MODEL_PRIMARY_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width, modulus):
 * entries is NO_TABLE for a C that takes no command index; otherwise the setup command
 * must be below it (taken modulo `modulus` when that is not 0), and an update's entry (at `field` in the work area) covered: with
 * index_width 0 a pointer to one of the `entries` of `stride` bytes from `table` (offsets
 * from the slot base), else the stored command itself, a u8, u16 or u32, below `entries`. */
MODEL_PRIMARY_ROW(0, 38u, 0x148u, "9308f7495ef02658ce22a8dad1d92584191f7e9899b5ce7c818b5e832833143b", model_primary_38__func_8013A004, 0x3u, 0x118u, 0x10u, 0x0u, 0u, 0u) /* shape 38 */
MODEL_PRIMARY_ROW(1, 41u, 0x148u, "7b39c99578ffd4c9518062db359c0b78720e7c724b5f4f063ccd6cba04236cd3", model_primary_38_slot1__func_8017A004, 0x3u, 0x118u, 0x10u, 0x0u, 0u, 0u) /* shape 38 */
MODEL_PRIMARY_ROW(0, 39u, 0x10Cu, "751f328d58ad717b78b5f280453a109e333031fef644ecd82ee874f8816359ec", model_primary_39__func_8013A004, 0x3u, 0xDCu, 0x10u, 0x0u, 0u, 100u) /* shape 39 */
MODEL_PRIMARY_ROW(1, 42u, 0x10Cu, "e589d3bb5d18d3c3e8780e8583dcc6acac01bb9027cd1002ce81185a96853bae", model_primary_39_slot1__func_8017A004, 0x3u, 0xDCu, 0x10u, 0x0u, 0u, 100u) /* shape 39 */
MODEL_PRIMARY_ROW(0, 45u, 0x1000u, "10e33095b4b2d5fd7de1e43354c8a0daf101e1b5f0ed621a849ea037d957920b", model_primary_45__func_8013A004, 0x1u, 0x750u, 0x10u, 0x0u, 0u, 0u) /* shape 45 */
MODEL_PRIMARY_ROW(1, 49u, 0x1000u, "4fe65bf8455d4e35762500f79ee72f5985acdf4299e8444e1a59111b40b6f62d", model_primary_45_slot1__func_8017A004, 0x1u, 0x750u, 0x10u, 0x0u, 0u, 0u) /* shape 45 */
MODEL_PRIMARY_ROW(0, 46u, 0x1000u, "6463eb9fec2f1984d3d49a217b80a4a9d23bf41b90e10195c466b43992c75ffb", model_primary_46__func_8013A004, 0x1u, 0xA18u, 0xCu, 0x0u, 0u, 0u) /* shape 46 */
MODEL_PRIMARY_ROW(1, 50u, 0x1000u, "cbb5dae73871869f4e4cc3110647a71fb8615e9848605bfb39987322975ada82", model_primary_46_slot1__func_8017A004, 0x1u, 0xA18u, 0xCu, 0x0u, 0u, 0u) /* shape 46 */
MODEL_PRIMARY_ROW(0, 47u, 0x1000u, "52fe8f80cb96cfd55301a91938a5e9c686100511090a83f64a964d5aecc41877", model_primary_47__func_8013A004, 0x1u, 0xA30u, 0x5Au, 0x0u, 0u, 0u) /* shape 47 */
MODEL_PRIMARY_ROW(1, 51u, 0x1000u, "24b7db963d8569f3135a57efe691e51446dc0cc9ac1fe409ce79bb6dc5d85a2c", model_primary_47_slot1__func_8017A004, 0x1u, 0xA30u, 0x5Au, 0x0u, 0u, 0u) /* shape 47 */
