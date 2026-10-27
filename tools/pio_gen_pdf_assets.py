Import("env")
import os
import subprocess
import sys

root = env.subst("$PROJECT_DIR")
subprocess.check_call([sys.executable, os.path.join(root, "tools", "gen_pdf_assets.py")])
