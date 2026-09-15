"""Build the unsigned NoTrpDrm-compatible Vita trophy archive.

The format is TRP v2 (the PS3/Vita variant).  It intentionally contains raw
TROP.SFM XML: NoTrpDrm supplies the homebrew signature bypass on the console.
"""
from __future__ import annotations

import argparse
import hashlib
import struct
import zlib
from pathlib import Path
from xml.sax.saxutils import escape

COMM_ID = "BANJ00064_00"

# Display order, names, descriptions and point values mirror the 60-entry
# RetroAchievements base set for game 10210. Vita ID 000 is the platinum.
RA_TROPHIES = [
    ("Banjo", "Open the 50 note door", 3),
    ("Kazooie", "Open the 180 note door", 4),
    ("Flute", "Open the 260 note door", 5),
    ("Violin", "Open the 350 note door", 5),
    ("Saxophone", "Open the 450 note door", 5),
    ("Xylophone", "Open the 640 note door", 5),
    ("Welcome To The World's Greatest Game Show", "Open the 765 note door", 10),
    ("Spiral Mountain", "Get all Honeycomb Pieces in Spiral Mountain.", 5),
    ("Banjo the Termite", "Transform into Termite Banjo at Mumbo's Mountain.", 3),
    ("Mumbo's Mountain", "Collect all Jiggies, Notes, and Hollow Honeycombs in Mumbo's Mountain.", 10),
    ("To infinity and beyond!", "Reach the top of Treasure Trove Cove.", 3),
    ("Treasure Trove Cove", "Collect all Jiggies, Notes, and Hollow Honeycombs in Treasure Trove Cove.", 10),
    ("Steel Lungs", "Release Clanker without surfacing for air or touching any underwater bubbles after talking with him.", 5),
    ("Clanker's Cavern", "Collect all Jiggies, Notes, and Hollow Honeycombs in Clanker's Cavern.", 10),
    ("Banjo the Crocodile", "Transform into Crocodile Banjo in Bubblegloop Swamp.", 3),
    ("God Tier", "Win Mr. Vile's second challenge without using Turbo Trainers.", 10),
    ("Bubblegloop Swamp", "Collect all Jiggies, Notes, and Hollow Honeycombs in Bubblegloop Swamp.", 10),
    ("Banjo the Cute Walrus", "Tranform into Walrus Banjo at Freezeezy Peak.", 3),
    ("Freezeezy Peak", "Collect all Jiggies, Notes, and Hollow Honeycombs in Freezeezy Peak.", 10),
    ("Gobi's Valley", "Collect all Jiggies, Notes, and Hollow Honeycombs in Gobi's Valley.", 10),
    ("Banjo the Pumpkin", "Transform into Pumpkin Banjo at Mad Monster Mansion.", 3),
    ("Mad Monster Mansion", "Collect all Jiggies, Notes, and Hollow Honeycombs in Mad Monster Mansion.", 10),
    ("Rusty Bucket Bay", "Collect all Jiggies, Notes, and Hollow Honeycombs in Rusty Bucket Bay.", 10),
    ("Banjo wants honey", "Transform into Bee Banjo at Click Clock Wood.", 3),
    ("Click Clock Wood", "Collect all Jiggies, Notes, and Hollow Honeycombs in Click Clock Wood.", 10),
    ("Show me the honey!", "Collect all 24 honeycomb pieces and the upgrade. Yes! Every single one! It's character-building.", 10),
    ("Warping Around", "Activate all of the Warp Cauldrons in Grunty's Lair", 5),
    ("Grunty's Lair", "Collect all 10 Jiggies in Grunty's Lair.", 10),
    ("Pieces of a puzzle", "Collect all Jigsaw Pieces.", 25),
    ("Witch Exterminator", "Defeat Grunty without taking damage.", 50),
    ("Jinjonatored", "Fear the Jinjonator. If you're a witch, that is.", 10),
    ("What's a Hollow Honeycomb?", "Complete the game without extending the Life Bar.", 25),
    ("I just wanted to save...", "Get Monster Tooty Ending.", 1),
    ("The Quiz Master", "Complete Grunty's Furnace Fun quiz show and win the Star Prize.", 5),
    ("Mystery Unsolved", "Get the secret ending in Banjo Kazooie.", 10),
    ("Jigsaw Maker", "Solve all seven of Bottles' moving picture puzzles.", 10),
    ("Move Master", "Discover and master all Banjo and Kazooie moves.", 10),
    ("I'm the Egg Cheater", "Unlock the cheat Blue Eggs.", 5),
    ("Fine Feathered Cheat", "Unlock the cheat Red Feathers.", 5),
    ("The Last Cheat", "Unlock the cheat Gold Feathers.", 5),
    ("Bright Skull", "Collect all Mumbo tokens in the game.", 25),
    ("Stoped and Swoped", "Get all Stop'n'Swop items.", 10),
    ("Lives Collector", "Get 9 lives.", 5),
    ("In Spiral Mountain, Fields Are Green", "Collect every 1up in Spiral Mountain in a single life without playing Bottles' puzzle minigame.", 5),
    ("Surrounded by Sea", "Collect every 1up in Mumbo's Mountain in a single life.", 5),
    ("Treasure Trove Cove's Got a Treasure Hunt", "Collect every 1up in Treasure Trove Cove in a single life.", 5),
    ("Inside Clanker spinning fast", "Collect every 1up in Clanker's Cavern in a single life.", 5),
    ("Bubblegloop Turtle Choir Is Swelling", "Collect every required 1up in Bubblegloop Swamp in a single life.", 5),
    ("Freezeezy's Igloo Isn't Square", "Collect every 1up in Freezeezy Peak in a single life.", 5),
    ("Gobi's Valley Is Easy for Sure", "Collect every 1up in Gobi's Valley in a single life.", 5),
    ("Monster Mansion Gives You a Scare", "Collect every 1up in Mad Monster Mansion in a single life.", 5),
    ("The Rusty Bucket Is a Tanker", "Collect every 1up in Rusty Bucket Bay in a single life.", 5),
    ("Click Clock Wood's Winter Is Dull", "Collect every 1up in Click Clock Wood in a single life.", 10),
    ("Is It This, Now Let Me Think, What Is Grunty's Favorite Drink?", "Collect every 1up outside Furnace Fun in Gruntilda's Lair in a single life without transformations.", 10),
    ("Banjo the Washer", "Transform into Washer Banjo.", 5),
    ("Big Head Banjo", "Enter bottlesbonusone in the sandcastle.", 5),
    ("Big Hands and Feets", "Enter bottlesbonustwo in the sandcastle.", 5),
    ("Big Kazooie", "Enter bottlesbonusthree in the sandcastle.", 5),
    ("Slim Banjo", "Enter bottlesbonusfour in the sandcastle.", 5),
    ("Strange Banjo", "Enter bottlesbonusfive in the sandcastle.", 5),
]

NTSC_GLITCH_TROPHIES = [
    ("Bear of the Mountain [NTSC]", "In Mumbo's Mountain, obtain the Jiggy on top of the termite hill without turning into a termite", 5),
    ("Kazooie Hill [NTSC]", "Obtain the Jiggy on top of Mumbo Mountain's door without turning into a termite", 5),
    ("Quick Dive [NTSC]", "Obtain the underwater Honeycomb in Treasure Trove Cove without pressing B to dive underwater", 5),
    ("Underwater Dentistry [NTSC]", "In Clanker's Cavern, obtain the toothache Jiggy without raising Clanker to the surface", 5),
    ("Gesundheit [NTSC]", "In Gobi's Valley, obtain the Jiggy inside the sphinx without opening the door", 10),
    ("Jail Break [NTSC]", "Obtain the Jiggy inside Grunty's statue without pressing the Bubblegloop Swamp Grunty switch", 5),
    ("Mummified [NTSC]", "Obtain the Jiggy in the sarcophagus without pressing the Gobi's Valley Grunty switch", 5),
    ("Swimming With the Fishes [NTSC]", "Near Bubblegloop Swamp, swim in the piranha water and collect a golden feather while below maximum", 5),
    ("Well Well Well [NTSC]", "In Mad Monster Mansion, obtain the well Jiggy without pressing B to dive underwater", 5),
    ("Beaver Bother [NTSC]", "Open Gnawty's house in Spring and swim to the top without pressing the Summer, Fall, or Winter switch", 5),
    ("Furnace Fun Skip [NTSC]", "Perform the Furnace Fun Skip and stand next to Tooty", 10),
]

def grade(points: int) -> str:
    return "G" if points >= 25 else "S" if points >= 10 else "B"

TROPHIES = [(0, "P", -1, None, "The Bear and Bird's Shining Legacy",
             "Earn every Banjo-Kazooie trophy.")] + [
    (index, grade(points), 0, None, name, detail)
    for index, (name, detail, points) in enumerate(RA_TROPHIES, 1)
]+ [
    (index, grade(points), -1, 1, name, detail)
    for index, (name, detail, points) in enumerate(NTSC_GLITCH_TROPHIES, 61)
]


def png(width: int, height: int, pixels: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def decode_png_rgba(path: Path) -> tuple[int, int, bytes]:
    """Decode the small non-interlaced 8-bit PNGs used by the Vita assets."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {path}")
    pos, payloads = 8, {}
    while pos < len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        payloads[kind] = payloads.get(kind, b"") + payload
        pos += 12 + length
    width, height, depth, color, compression, filtering, interlace = struct.unpack(">IIBBBBB", payloads[b"IHDR"])
    if depth != 8 or compression or filtering or interlace or color not in (2, 3, 6):
        raise ValueError(f"unsupported PNG format in {path}")
    bpp = 1 if color == 3 else 3 if color == 2 else 4
    packed = zlib.decompress(payloads[b"IDAT"])
    rows, cursor, previous = [], 0, bytearray(width * bpp)
    for _ in range(height):
        filter_type, scanline = packed[cursor], bytearray(packed[cursor + 1:cursor + 1 + width * bpp])
        cursor += 1 + width * bpp
        for i, value in enumerate(scanline):
            left = scanline[i - bpp] if i >= bpp else 0
            up = previous[i]
            upper_left = previous[i - bpp] if i >= bpp else 0
            if filter_type == 1:
                scanline[i] = (value + left) & 0xff
            elif filter_type == 2:
                scanline[i] = (value + up) & 0xff
            elif filter_type == 3:
                scanline[i] = (value + ((left + up) >> 1)) & 0xff
            elif filter_type == 4:
                p, pa, pb, pc = left + up - upper_left, abs(up - upper_left), abs(left - upper_left), abs(left + up - 2 * upper_left)
                scanline[i] = (value + (left if pa <= pb and pa <= pc else up if pb <= pc else upper_left)) & 0xff
            elif filter_type != 0:
                raise ValueError(f"unsupported PNG filter in {path}")
        rows.append(scanline)
        previous = scanline
    if color == 6:
        return width, height, bytes().join(rows)
    if color == 2:
        rgb = bytes().join(rows)
        pixels = bytearray(width * height * 4)
        for i in range(width * height):
            pixels[i * 4:i * 4 + 3] = rgb[i * 3:i * 3 + 3]
            pixels[i * 4 + 3] = 255
        return width, height, bytes(pixels)
    palette, alpha = payloads[b"PLTE"], payloads.get(b"tRNS", b"")
    pixels = bytearray(width * height * 4)
    for pixel_index, palette_index in enumerate(bytes().join(rows)):
        source, target = palette_index * 3, pixel_index * 4
        pixels[target:target + 4] = palette[source:source + 3] + bytes((alpha[palette_index] if palette_index < len(alpha) else 255,))
    return width, height, bytes(pixels)


def resize_png(path: Path, width: int, height: int) -> bytes:
    source_width, source_height, source = decode_png_rgba(path)
    output = bytearray(width * height * 4)
    for y in range(height):
        source_y = y * source_height // height
        for x in range(width):
            source_x = x * source_width // width
            target = (y * width + x) * 4
            start = (source_y * source_width + source_x) * 4
            output[target:target + 4] = source[start:start + 4]
    return png(width, height, bytes(output))


def xml(configuration_only: bool = False) -> bytes:
    # NoTrpDrm recognizes the conventional 160-byte development placeholder
    # (encoded here as 320 literal `x` characters). This exact representation
    # is used by working unsigned Vita trophy packs; a fabricated hex record
    # is still parsed as a real signature and is rejected by the setup dialog.
    signature = "x" * 320
    lines = [
        f'<!--Sce-Np-Trophy-Signature: {signature}-->',
        '<trophyconf version="1.1" platform="psp2" policy="large">',
        f' <npcommid>{COMM_ID}</npcommid>',
        ' <trophyset-version>01.03</trophyset-version>',
        ' <parental-level license-area="default">0</parental-level>',
    ]
    if not configuration_only:
        lines.extend((
            ' <title-name>Banjo-Kazooie</title-name>',
            ' <title-detail>The Lighthouse port trophy set, based on the RetroAchievements game 10210 base set.</title-detail>',
            ' <group id="001">',
            '  <name>Glitchy Fun</name>',
            '  <detail>NTSC achievements from the RetroAchievements Glitch Showcase subset.</detail>',
            ' </group>',
        ))
    else:
        lines.append(' <group id="001"/>')
    for tid, grade, parent, group, name, detail in TROPHIES:
        attrs = f'id="{tid:03d}" hidden="no" ttype="{grade}" pid="{parent:03d}"'
        if parent < 0:
            attrs = f'id="{tid:03d}" hidden="no" ttype="{grade}" pid="-1"'
        if group is not None:
            attrs += f' gid="{group:03d}"'
        if configuration_only:
            lines.append(f' <trophy {attrs}/>')
        else:
            lines.extend((f' <trophy {attrs}>', f'  <name>{escape(name)}</name>', f'  <detail>{escape(detail)}</detail>', ' </trophy>'))
    lines.append('</trophyconf>')
    return ('\n'.join(lines) + '\n').encode('utf-8')


def trp(files: dict[str, bytes]) -> bytes:
    # PS Vita uses the big-endian v2 header, 0x40 bytes, with 0x40-byte TOC rows.
    # The final 16 bytes of the packed header above are reserved; they are part
    # of the header, not a prefix before the table of contents.
    header_size, entry_size = 0x40, 0x40
    # Keep the conventional Vita order: the compact configuration manifest
    # must lead the archive, immediately followed by its localized metadata.
    # Working NoTrpDrm packs use this order; alphabetical sorting would put
    # TROPCONF.SFM last after the image assets.
    entries = list(files.items())
    offset = header_size + len(entries) * entry_size
    table, bodies = [], []
    for name, payload in entries:
        offset = (offset + 15) & ~15
        bodies.append((offset, payload))
        table.append(name.encode('ascii').ljust(32, b'\0') + struct.pack('>QQI12x', offset, len(payload), 0))
        offset += len(payload)
    image = bytearray(offset)
    image[:header_size] = struct.pack('>IIQIII20s16x', 0xDCA24D00, 2, offset, len(entries), entry_size, 0, b'\0' * 20)
    image[header_size:header_size + len(entries) * entry_size] = b''.join(table)
    for position, payload in bodies:
        image[position:position + len(payload)] = payload
    digest = hashlib.sha1(image).digest()
    image[0x1c:0x30] = digest
    return bytes(image)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--livearea-icon', type=Path, required=True)
    args = parser.parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    platinum = Path(__file__).parent.parent / 'vita' / 'trophy' / 'platinum.png'
    trophy_assets = Path(__file__).parent.parent / 'vita' / 'trophy' / 'achievements'
    missing_assets = [
        ((trophy_assets / 'glitch_ntsc' / f'{tid:03d}.png')
         if tid >= 61 else (trophy_assets / f'{tid:03d}.png'))
        for tid, *_ in TROPHIES
        if tid != 0 and not ((trophy_assets / 'glitch_ntsc' / f'{tid:03d}.png')
                            if tid >= 61 else (trophy_assets / f'{tid:03d}.png')).is_file()
    ]
    if missing_assets:
        names = ', '.join(path.name for path in missing_assets)
        raise FileNotFoundError(
            f'missing trophy artwork in {trophy_assets}: {names}'
        )
    # Vita's setup dialog first consumes the compact configuration manifest,
    # then reads the localized trophy metadata. Without TROPCONF.SFM it reports
    # NP-6182-7 even when the archive itself is present at the correct path.
    files = {
        'TROPCONF.SFM': xml(configuration_only=True),
        'TROP.SFM': xml(),
        # Trophy assets have fixed Vita dimensions: title/group images are
        # 320x176 and individual trophy images are 240x240. LiveArea's icon
        # is only 128x128, so never insert it into the archive verbatim.
        'ICON0.PNG': resize_png(args.livearea_icon, 320, 176),
        'GR001.PNG': resize_png(trophy_assets / 'glitch_ntsc' / '061.png', 320, 176),
    }
    for tid, *_ in TROPHIES:
        asset = ((trophy_assets / 'glitch_ntsc' / f'{tid:03d}.png')
                 if tid >= 61 else (trophy_assets / f'{tid:03d}.png'))
        files[f'TROP{tid:03d}.PNG'] = (
            resize_png(platinum, 240, 240) if tid == 0 else resize_png(asset, 240, 240)
        )
    args.out.write_bytes(trp(files))
    print(f'wrote {args.out} ({args.out.stat().st_size} bytes, {len(TROPHIES)} trophies, {COMM_ID})')


if __name__ == '__main__':
    main()
