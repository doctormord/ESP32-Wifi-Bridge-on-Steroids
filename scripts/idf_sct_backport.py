"""PlatformIO pre-build script: enable the SPI master's segmented transfer
mode (SCT) for the ESP32-C5 in the installed ESP-IDF 5.5 framework.

ESP-IDF 5.5 implements SCT in the SPI master driver and the HAL for all chips,
but only the chips whose soc_caps.h sets SOC_SPI_SCT_SUPPORTED compile it in,
and the ESP32-C5 is not among them although its GP-SPI2 has the same SCT
hardware as the ESP32-C6 (same register list in the conf buffer, same magic
value - ESP-IDF master enables it for the C5). This script adds the two
missing pieces to the framework package, idempotently:

  - the SOC_SPI_SCT_* capabilities in components/soc/esp32c5/.../soc_caps.h
    (values as for the C6);
  - the SCT low-level functions in components/hal/esp32c5/.../spi_ll.h,
    taken unchanged from the C6 header of the same IDF version
    (patches/idf-esp32c5-spi-sct/spi_ll_sct.h).

Both insertions are bracketed by marker comments, so a second run finds them
and does nothing. Only C5 files are touched; other targets build unchanged.
The framework package is shared by all PlatformIO projects on this machine;
the patch only enables code that is otherwise compiled out.
"""
import os

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

MARK_BEGIN = "/* BEGIN esp32c5 SCT backport (esp32_wifi_bridge) */"
MARK_END = "/* END esp32c5 SCT backport */"

CAPS = """
#define SOC_SPI_SCT_SUPPORTED                     1
#define SOC_SPI_SCT_SUPPORTED_PERIPH(PERIPH_NUM)  ((PERIPH_NUM==1) ? 1 : 0)    //Support Segmented-Configure-Transfer
#define SOC_SPI_SCT_REG_NUM                       14
#define SOC_SPI_SCT_BUFFER_NUM_MAX                (1 + SOC_SPI_SCT_REG_NUM)  //1-word-bitmap + 14-word-regs
#define SOC_SPI_SCT_CONF_BITLEN_MAX               0x3FFFA       //18 bits wide reg
"""


def insert(path, anchor, block, after):
    """Insert 'block' (wrapped in the markers) before or after the first line
    containing 'anchor' in 'path', unless the markers are already there."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if MARK_BEGIN in text:
        return False
    lines = text.split("\n")
    idx = next((i for i, l in enumerate(lines) if anchor in l), None)
    if idx is None:
        raise RuntimeError("SCT backport: anchor '%s' not found in %s" % (anchor, path))
    wrapped = [MARK_BEGIN] + block.strip("\n").split("\n") + [MARK_END]
    pos = idx + 1 if after else idx
    lines[pos:pos] = wrapped
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    return True


def apply():
    """Patch the framework if this environment targets the ESP32-C5."""
    if env.BoardConfig().get("build.mcu", "") != "esp32c5":  # noqa: F821
        return
    fw = env.PioPlatform().get_package_dir("framework-espidf")  # noqa: F821
    root = env.subst("$PROJECT_DIR")  # noqa: F821
    with open(os.path.join(root, "patches", "idf-esp32c5-spi-sct", "spi_ll_sct.h"),
              encoding="utf-8") as f:
        ll_block = f.read()
    caps = os.path.join(fw, "components", "soc", "esp32c5", "include", "soc", "soc_caps.h")
    ll = os.path.join(fw, "components", "hal", "esp32c5", "include", "hal", "spi_ll.h")
    changed = insert(caps, "#define SOC_SPI_SUPPORT_CLK_RC_FAST", CAPS, after=True)
    changed |= insert(ll, "#undef SPI_LL_RST_MASK", ll_block, after=False)
    if changed:
        print("SCT backport: enabled SPI SCT mode for esp32c5 in " + fw)


apply()
