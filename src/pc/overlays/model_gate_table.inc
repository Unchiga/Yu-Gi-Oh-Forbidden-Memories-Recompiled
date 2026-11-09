/* Written by tools/pc/model_gate.py from config/pc/model_gate.csv: do not edit.
 * One row for each variant module the decomp's C stands in for:
 * MODEL_GATE_ROW(slot, identifier, span, sha256, entry, entries, table, stride, field, index_width):
 * entries is NO_TABLE for a C that takes no command index; otherwise the setup command
 * must be below it, and an update's entry (at `field` in the work area) covered: with
 * index_width 0 a pointer to one of the `entries` of `stride` bytes from `table` (offsets
 * from the slot base), else the stored command itself, a u8, u16 or u32, below `entries`. */
MODEL_GATE_ROW(0, 59u, 0xBE0u, "bedd129804fd7c233af04c7aa594c57c86eb61752067898550bd32974f0d7743", model_variant_59__func_8013B004, 0x3u, 0xB98u, 0x18u, 0x0u, 0u) /* shape 59 */
MODEL_GATE_ROW(1, 189u, 0xBE0u, "de245c85e49481d8fba6c98ce2e76d29da3940c79a23c87f40799900f1d195dc", model_variant_59_slot1__func_8017B004, 0x3u, 0xB98u, 0x18u, 0x0u, 0u) /* shape 59 */
MODEL_GATE_ROW(0, 65u, 0xF0Cu, "8270e81f65269309d4e439de37c7331755c3b9ed3b44c9513e8a444ca69b2539", model_variant_65__func_8013B004, 0x9u, 0xE10u, 0x1Cu, 0x0u, 0u) /* shape 65 */
MODEL_GATE_ROW(1, 195u, 0xF0Cu, "aa43a2b8bc1d8ddbcaf13b71bb857b00200e1f5820714b1ee37b98caa745f645", model_variant_65_slot1__func_8017B004, 0x9u, 0xE10u, 0x1Cu, 0x0u, 0u) /* shape 65 */
MODEL_GATE_ROW(0, 79u, 0xCDCu, "61b67882180aa432129f18113d05c5bbe86ff69f4cd2b5ed0dbe0d0164b38d79", model_variant_79__func_8013B004, 0x5u, 0xC78u, 0x14u, 0x0u, 0u) /* shape 79 */
MODEL_GATE_ROW(1, 209u, 0xCDCu, "01c42824211420234182308c2a525816dafc12d224dd170d6462b10c7883ab20", model_variant_79_slot1__func_8017B004, 0x5u, 0xC78u, 0x14u, 0x0u, 0u) /* shape 79 */
MODEL_GATE_ROW(0, 145u, 0xE18u, "238c750db4a9467fa6cd9ebc876f8c848717c5234dbf2d4ef4061a5b5328fbe3", model_variant_145__func_8013B004, 0x1u, 0xE00u, 0x14u, 0x0u, 0u) /* shape 145 */
MODEL_GATE_ROW(1, 275u, 0xE18u, "ae4b2ba6e9747c3ee860cf8b245b85a7d8d75ed524de95be8a28b05726a1ab33", model_variant_145_slot1__func_8017B004, 0x1u, 0xE00u, 0x14u, 0x0u, 0u) /* shape 145 */
MODEL_GATE_ROW(0, 376u, 0x2654u, "3c7e4ac05a54a96b43937e521344cd97809593518a4db25feac34bf1e1709458", model_variant_376__func_8013B004, 0x8u, 0x2434u, 0x44u, 0xDB4u, 0u) /* shape 376 */
MODEL_GATE_ROW(1, 526u, 0x2654u, "914af4b3d8438ccc6cd9d50da127a21289fc0ebd303a799269ea2228a3b80c81", model_variant_376_slot1__func_8017B004, 0x8u, 0x2434u, 0x44u, 0xDB4u, 0u) /* shape 376 */
MODEL_GATE_ROW(0, 407u, 0x1D7Cu, "7001e0f61818b6a6be96843c1a411932b924975bcbb606255cb54c063cd9ea3d", model_variant_407__func_8013B004, 0xEu, 0x19C4u, 0x44u, 0x29D4u, 0u) /* shape 407 */
MODEL_GATE_ROW(1, 557u, 0x1D7Cu, "fa521dada008ca295f2413f06f113fef1182f11992ea17829d42c449a178ee88", model_variant_407_slot1__func_8017B004, 0xEu, 0x19C4u, 0x44u, 0x29D4u, 0u) /* shape 407 */
MODEL_GATE_ROW(0, 415u, 0x1F7Cu, "66b1a8c0c700253dd8003881c7e46e33f791662b35bc66d9d5a9a9a1f81ec90c", model_variant_415__func_8013B004, 0xFFFFFFFFu, 0u, 0u, 0u, 0u) /* shape 415 */
MODEL_GATE_ROW(1, 565u, 0x1F7Cu, "b98ecf60af0f4f149240a1d856bc1fdb51d9ab64a2269a045ef62784236509b3", model_variant_415_slot1__func_8017B004, 0xFFFFFFFFu, 0u, 0u, 0u, 0u) /* shape 415 */
MODEL_GATE_ROW(0, 418u, 0x3F60u, "410af68bb5a48427301e8a530f1994a90e40ae31cbe025698ea81fe3a5dbfe14", model_variant_418__func_8013B004, 0x12u, 0x3A68u, 0x44u, 0x1B74u, 0u) /* shape 418 */
MODEL_GATE_ROW(1, 568u, 0x3F60u, "d5c172ab1b1b4ce4e00fb6ee86022deb1b8be7ed95c05a0f9de61cc52585c5d8", model_variant_418_slot1__func_8017B004, 0x12u, 0x3A68u, 0x44u, 0x1B74u, 0u) /* shape 418 */
MODEL_GATE_ROW(0, 433u, 0x39FCu, "8f785e169ce0776bf27a4d92f8aa1ae50bb7858d27f89fbc9987db5544c35003", model_variant_433__func_8013B004, 0x1u, 0x39D8u, 0x24u, 0x42BCu, 0u) /* shape 433 */
MODEL_GATE_ROW(1, 583u, 0x39FCu, "d9e304c38c591177238086270fa635d545535379e16cf490149bdfe4e0352da1", model_variant_433_slot1__func_8017B004, 0x1u, 0x39D8u, 0x24u, 0x42BCu, 0u) /* shape 433 */
MODEL_GATE_ROW(0, 443u, 0x2D04u, "fa8e99959d93b29526e18e60aeb2cd0c23ac1685fe1c013db855b1122280986e", model_variant_443__func_8013B004, 0xEu, 0x2984u, 0x40u, 0x2E7Cu, 0u) /* shape 443 */
MODEL_GATE_ROW(1, 593u, 0x2D04u, "3e4d6a514d19d3becfa9556073097c13119f61e3969b89e9babdb71659f63f95", model_variant_443_slot1__func_8017B004, 0xEu, 0x2984u, 0x40u, 0x2E7Cu, 0u) /* shape 443 */
MODEL_GATE_ROW(0, 648u, 0x1B84u, "ce54d63a7d5dc9e6034282cf2e6cdd8aa4c04ed0f6f706bfd5fcc177e49351e9", model_variant_648__func_8013B004, 0xFFFFFFFFu, 0u, 0u, 0u, 0u) /* shape 648 */
MODEL_GATE_ROW(1, 715u, 0x1B84u, "1a44c602e2d46eba1eb4109a36475989dfe120e195b7fca8f3f39b9abe16732e", model_variant_648_slot1__func_8017B004, 0xFFFFFFFFu, 0u, 0u, 0u, 0u) /* shape 648 */
MODEL_GATE_ROW(0, 655u, 0x4D94u, "994663320142a945bcaa31db5f89c7bd24236cd7446482fe2fccb3eaebfd2537", model_variant_655__func_8013B004, 0xFFFFFFFFu, 0u, 0u, 0u, 0u) /* shape 655 */
MODEL_GATE_ROW(1, 722u, 0x4D94u, "fc8dabefac568998d36b609d6826d3ac75980effd01b43110f45dd6565538537", model_variant_655_slot1__func_8017B004, 0xFFFFFFFFu, 0u, 0u, 0u, 0u) /* shape 655 */
MODEL_GATE_ROW(0, 661u, 0x3E2Cu, "0282aeabc12a832799744fffe2e77cb71784edeecc0eb604f2a7353c2d787bec", model_variant_661__func_8013B004, 0xFFFFFFFFu, 0u, 0u, 0u, 0u) /* shape 661 */
MODEL_GATE_ROW(1, 728u, 0x3E2Cu, "7c3bc2008800a75080292d79ff7b731833982d2aa391a31e0e42430ca2599f69", model_variant_661_slot1__func_8017B004, 0xFFFFFFFFu, 0u, 0u, 0u, 0u) /* shape 661 */
MODEL_GATE_ROW(0, 663u, 0x3354u, "39fa781737b3c7e5a00b58efc53c070d4f5715bfc709842d3d65c28635c9f5ab", model_variant_663__func_8013B004, 0xFFFFFFFFu, 0u, 0u, 0u, 0u) /* shape 663 */
MODEL_GATE_ROW(1, 730u, 0x3354u, "30be7bc88c01e7a18fd40d03a753fcb6a2e8bed4bd1333416fd1518eff4beb7e", model_variant_663_slot1__func_8017B004, 0xFFFFFFFFu, 0u, 0u, 0u, 0u) /* shape 663 */
MODEL_GATE_ROW(0, 752u, 0x1D58u, "c8feedcf0e5db4790171b4b8c0d10fa0a245371a2a48ccea34f70fbe6fa5b1de", model_variant_752__func_8013B004, 0xDu, 0x19E4u, 0x44u, 0x0u, 0u) /* shape 752 */
MODEL_GATE_ROW(1, 849u, 0x1D58u, "7d84a81739938670170911d1a4f5223fcd018dd4c0233b1033b1d06b52961ab9", model_variant_752_slot1__func_8017B004, 0xDu, 0x19E4u, 0x44u, 0x0u, 0u) /* shape 752 */
MODEL_GATE_ROW(0, 754u, 0x1F18u, "f0e0895f043a6b7f55103b0b0a87f67231259901e6c6cce703c66f370c2df6a6", model_variant_754__func_8013B004, 0x13u, 0x19C0u, 0x48u, 0x0u, 0u) /* shape 754 */
MODEL_GATE_ROW(1, 851u, 0x1F18u, "a69c29cd5a323cf56c53df68c32578e74ac1f8cc5f93e30b46b0011ffe99ec84", model_variant_754_slot1__func_8017B004, 0x13u, 0x19C0u, 0x48u, 0x0u, 0u) /* shape 754 */
MODEL_GATE_ROW(0, 756u, 0x25ACu, "c0a325735da8da44ca9922ca02a2c23a9425332ee0d49b0b6654100ff8e23890", model_variant_756__func_8013B004, 0x1Au, 0x2064u, 0x34u, 0x0u, 0u) /* shape 756 */
MODEL_GATE_ROW(1, 853u, 0x25ACu, "b19a03f44f81feeab8735a92881e60f1696a03b26e98a76d143c60300553967a", model_variant_756_slot1__func_8017B004, 0x1Au, 0x2064u, 0x34u, 0x0u, 0u) /* shape 756 */
MODEL_GATE_ROW(0, 766u, 0x1D90u, "0043d5bf51d94d263efeab3f8753c103709ed8876cef0ad04f21a6a7f5bdca5a", model_variant_766__func_8013B004, 0x1Bu, 0x1880u, 0x30u, 0x0u, 0u) /* shape 766 */
MODEL_GATE_ROW(1, 863u, 0x1D90u, "045695d38d05afb727780b297910bbbe30b2a1f1f2c92e8a423d8f58ed1c142d", model_variant_766_slot1__func_8017B004, 0x1Bu, 0x1880u, 0x30u, 0x0u, 0u) /* shape 766 */
MODEL_GATE_ROW(0, 777u, 0x26D4u, "6b49285e3fcf78a81c509dbcb07eb494ecadd7979fa9a6eadde1b5ff51b8eca9", model_variant_777__func_8013B004, 0xCu, 0x2494u, 0x30u, 0x0u, 0u) /* shape 777 */
MODEL_GATE_ROW(1, 874u, 0x26D4u, "509e93776ab082614a1650e5552af99dbdf7dcab27307070a6e7aa25727702c2", model_variant_777_slot1__func_8017B004, 0xCu, 0x2494u, 0x30u, 0x0u, 0u) /* shape 777 */
MODEL_GATE_ROW(0, 828u, 0x1E3Cu, "6edfae473ae8fa069580f3f069a08ffdb2e472cd8e81a13c50bc30fc6dd00672", model_variant_828__func_8013B004, 0xFu, 0x1AB8u, 0x3Cu, 0x0u, 0u) /* shape 828 */
MODEL_GATE_ROW(1, 925u, 0x1E3Cu, "c48c8976d3979e6b9b3b6ae5f9c937f55897e3822edffad7f58a801f3b5e4a55", model_variant_828_slot1__func_8017B004, 0xFu, 0x1AB8u, 0x3Cu, 0x0u, 0u) /* shape 828 */
