#!/usr/bin/env python3
"""cornertest_run.py [--from 44] [--to 62] [--step 2] [--backlight 20] -- measure the panel's
rounded corners with tests/pbc/cornertest on a PicoBoy Color Plus in BOOTSEL and the IPEVO camera:
flashes the firmware, then over its USB serial port shows the arcs of each radius, photographs
each once the PicoBoy says it shows it, and evaluates the photos (cornertest_measure.py --arcs).
At the end the PicoBoy goes back to BOOTSEL."""
import argparse, glob, os, shutil, subprocess, sys, time
import serial

here = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("--from", dest="r0", type=int, default=44)
ap.add_argument("--to", dest="r1", type=int, default=62)
ap.add_argument("--step", type=int, default=2)
ap.add_argument("--backlight", type=int, default=20)
ap.add_argument("--out", default="/tmp/cornertest")
ap.add_argument("--crop", default="1124,108,2044,1148")
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)

drive = "/run/media/%s/RP2350" % os.environ.get("USER", "")
uf2 = os.path.join(here, "../tests/pbc/cornertest/build/cornertest.uf2")
if os.path.isdir(drive):
    shutil.copy(uf2, drive)
    os.sync()
port = None
for _ in range(60):
    ports = glob.glob("/dev/ttyACM*")
    if ports:
        port = ports[0]
        break
    time.sleep(0.5)
if not port:
    sys.exit("no USB serial port from the PicoBoy")
time.sleep(1)
s = serial.Serial(port, 115200, timeout=5)

def command(c):
    s.reset_input_buffer()
    s.write((c + "\n").encode())
    while True:
        line = s.readline().decode(errors="replace").strip()
        if not line:
            raise SystemExit("no answer to " + c)
        if line.startswith("ok") or line.startswith("?"):
            return line

def photo(path):
    # a few frames, so the camera's exposure has settled; keep the last
    tmp = path + ".%d.jpg"
    subprocess.run(["gst-launch-1.0", "-q", "v4l2src", "device=/dev/video4", "num-buffers=4", "!",
                    "image/jpeg,width=3264,height=2448", "!", "multifilesink", "location=" + tmp],
                   check=True, timeout=60)
    os.replace(tmp % 3, path)
    for i in range(3):
        try: os.remove(tmp % i)
        except OSError: pass

print(command("b %d" % a.backlight))
for r in range(a.r0, a.r1 + 1, a.step):
    print(command("r %d" % r), flush=True)
    time.sleep(0.5)
    p = os.path.join(a.out, "r%02d.jpg" % r)
    photo(p)
    res = subprocess.run([sys.executable, os.path.join(here, "cornertest_measure.py"), p, "--rotate180",
                          "--crop", a.crop, "--arcs", "--r", str(r)], capture_output=True, text=True)
    print((res.stdout.strip().splitlines() or [res.stderr.strip()])[-1], flush=True)
command("boot")
