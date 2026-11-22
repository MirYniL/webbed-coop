"""Entry point the game starts: WebbedCoop\\pythonw.exe WebbedCoop\\app\\coop.py --link <pid>."""

import asyncio
import sys

from link import link

if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--link":
        asyncio.run(link(int(sys.argv[2])))
