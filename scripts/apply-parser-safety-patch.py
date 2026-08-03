from pathlib import Path

SOURCE = Path("src/terra_wld.c")

OLD_LF = b"""    if (!ok||!terra_reader_has(*off,slen,len)){\n        if (cap)out[0]=0;\n        *off=len;\n        ;\n        }\n    uint32_t n=slen;"""
NEW_LF = b"""    if (!ok||!terra_reader_has(*off,slen,len)){\n        if (cap)out[0]=0;\n        *off=len;\n        return;\n        }\n    uint32_t n=slen;"""


def main() -> None:
    data = SOURCE.read_bytes()
    candidates = (
        (OLD_LF.replace(b"\n", b"\r\n"), NEW_LF.replace(b"\n", b"\r\n")),
        (OLD_LF, NEW_LF),
    )

    for old, new in candidates:
        count = data.count(old)
        if count == 1:
            SOURCE.write_bytes(data.replace(old, new, 1))
            print("patched rd_string_copy truncated-input return")
            return
        if count > 1:
            raise SystemExit(f"refusing to patch {count} matching reader blocks")

    if NEW_LF in data or NEW_LF.replace(b"\n", b"\r\n") in data:
        print("parser safety patch already applied")
        return

    raise SystemExit("expected rd_string_copy block was not found")


if __name__ == "__main__":
    main()
