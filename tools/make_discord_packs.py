# Makes the two optional packs shared on Discord (never on GitHub), from a game folder with our content:
#   dist\HCTP-Content-Pack.zip     <game>\MODS (backup_* / test_* folders left out) + HOST_MEDIA / MEDIA_DEFAULTS /
#                                  MEDIA_ROUTES.txt + ppv_sample.bin: models, entrances + lights, menus, music, trons
#   dist\HCTP-Commentary-Pack.zip  <game>\BGM files a stock disc doesn't have (Finkel / play-by-play)
# Both unzip into the player's game folder. Run: python tools\make_discord_packs.py [game folder] [stock BGM folder]
import os
import sys
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(REPO, 'dist')
GAME = sys.argv[1] if len(sys.argv) > 1 else r'F:\HCTP MODS\Vanilla'
STOCK_BGM = sys.argv[2] if len(sys.argv) > 2 else r'F:\hctp-newbase-vulcan\WWE SmackDown! Here Comes the Pain\BGM'

README_CONTENT = """HCTP Recompiled - Content Pack
Unzip INTO your game folder: HCTP Recompiled\\files\\game (the folder with SLUS_207.87). Say Yes to merge folders.
Adds: restored and converted models, entrances and lighting, menu names, music and TitanTrons, a sample PPV card.
Undo: delete the MODS folder and HOST_MEDIA.txt, MEDIA_DEFAULTS.txt, MEDIA_ROUTES.txt.
"""
README_COMMENTARY = """HCTP Recompiled - Commentary Pack
Unzip INTO your game folder: HCTP Recompiled\\files\\game (the folder with SLUS_207.87). Say Yes to merge folders.
The files go into BGM\\.
"""


def pack(name, items, readme):
    out = os.path.join(DIST, name)
    # stored, not compressed: ADX / SFD / PAC barely shrink and a 2 GB deflate takes far longer
    with zipfile.ZipFile(out + '.part', 'w', zipfile.ZIP_STORED, allowZip64=True) as z:
        z.writestr('README.txt', readme.replace('\n', '\r\n'))
        for src, arc in items:
            z.write(src, arc)
    os.replace(out + '.part', out)
    print(f'{name}: {len(items)} files, {os.path.getsize(out) / 1e6:.0f} MB')


def main():
    os.makedirs(DIST, exist_ok=True)
    items = []
    for root, dirs, files in os.walk(os.path.join(GAME, 'MODS')):
        dirs[:] = [d for d in dirs if not d.lower().startswith(('backup_', 'test_'))]
        for f in files:
            path = os.path.join(root, f)
            items.append((path, os.path.relpath(path, GAME)))
    for f in ('HOST_MEDIA.txt', 'MEDIA_DEFAULTS.txt', 'MEDIA_ROUTES.txt', 'ppv_sample.bin'):
        if os.path.isfile(os.path.join(GAME, f)):
            items.append((os.path.join(GAME, f), f))
    pack('HCTP-Content-Pack.zip', items, README_CONTENT)

    bgm = os.path.join(GAME, 'BGM')
    extra = [(os.path.join(bgm, f), 'BGM/' + f) for f in sorted(os.listdir(bgm))
             if os.path.isfile(os.path.join(bgm, f)) and not os.path.isfile(os.path.join(STOCK_BGM, f))]
    pack('HCTP-Commentary-Pack.zip', extra, README_COMMENTARY)


if __name__ == '__main__':
    main()
