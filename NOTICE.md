# Sources and licensing

This project is distributed under GNU GPL version 3; see [COPYING](COPYING).
Third-party files retain the license terms and notices supplied with those files.

- [OpenDisplay/Firmware](https://github.com/OpenDisplay/Firmware/tree/7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb), commit 7c9413edd9f7fa16e714f6ebc00b76efd3bad4eb:
  protocol/configuration reference and third_party/uzlib streaming decompressor.
  The decompressor was adapted to a small streaming sink; Huffman code lengths
  are packed into nibbles to reduce SRAM. Preserve the source copyright notices
  and [third_party/uzlib/LICENSE](third_party/uzlib/LICENSE). The source is derived
  from `lib/uzlib/src/` in that upstream revision. The adapted OpenDisplay stream
  file is covered by the upstream project license; underlying uzlib files retain
  their original notices.
- GDEW0213T5 Arduino P20201021 example supplied by the user:
  partial-refresh register settings, LUT values and inverted old/new pixel planes.
  This replaces the previous T5D-derived waveform. Source package:
  `A-GDEW0213T5-201021/GDEW0213T5_Arduino_P20201021/`.
  The original archive and example images are not redistributed.
- GDEW0213T5 Arduino example (2019-10-16): full-refresh hardware initialization
  sequence reference. The original archive and example images are not redistributed.
- Zephyr, CMSIS and Nordic HAL are fetched at revisions selected by [west.yml](west.yml) and its imported Zephyr manifest;
  each dependency retains its own license and copyright notices.
