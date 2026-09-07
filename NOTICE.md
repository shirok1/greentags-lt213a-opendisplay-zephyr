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
- [GxEPD2, GxEPD2_213_T5D.cpp](https://github.com/ZinggJM/GxEPD2/blob/master/src/epd/GxEPD2_213_T5D.cpp):
  reference for UC8151 partial refresh LUT values and experimental deep sleep.
  These values still need validation on the GDEW0213T5 panel.
- GDEW0213T5 Arduino example (2019-10-16): full-refresh hardware initialization
  sequence reference. The original archive and example images are not redistributed.
- Zephyr, CMSIS, Nordic HAL and TinyCrypt are fetched at commits in [west.yml](west.yml);
  each dependency retains its own license and copyright notices.
