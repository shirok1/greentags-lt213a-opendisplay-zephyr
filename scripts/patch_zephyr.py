"""Apply the no-log ATT timeout fix to the pinned Zephyr dependency."""
from pathlib import Path


def patch(root):
    path = root / "subsys/bluetooth/host/att.c"
    source = path.read_text()
    old = '\tbt_addr_le_to_str(bt_conn_get_dst(chan->att->conn), addr, sizeof(addr));\n\tLOG_ERR("ATT Timeout for device %s", addr);'
    new = '#if defined(CONFIG_LOG)\n\tchar addr[BT_ADDR_LE_STR_LEN];\n\n' + old + '\n#endif'
    declaration = '\tchar addr[BT_ADDR_LE_STR_LEN];\n\tstruct k_work_delayable *dwork'
    if new in source:
        return
    if source.count(old) != 1 or source.count(declaration) != 1:
        raise RuntimeError("Unexpected Zephyr ATT source; review timeout logging patch")
    source = source.replace(declaration, '\tstruct k_work_delayable *dwork')
    path.write_text(source.replace(old, new))


if __name__ == "__main__":
    patch(Path(__file__).resolve().parents[1] / ".deps/zephyr")
