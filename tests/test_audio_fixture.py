"""Integration fixture: nonzero container origin, 1 ms A/V offset, VFR grid."""
import array
import pathlib
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="hdrplay-sync-") as directory:
    root = pathlib.Path(directory)
    pcm = array.array("f", [0.0]) * (48000 * 2)
    pcm[11520 * 2] = 0.5
    pcm[11520 * 2 + 1] = -0.5
    (root / "audio.f32").write_bytes(pcm.tobytes())
    subprocess.run([
        sys.argv[2], "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "color=black:s=64x64:r=25:d=0.8",
        "-f", "f32le", "-ar", "48000", "-ac", "2", "-i", str(root / "audio.f32"),
        "-filter_complex",
        "[0:v]settb=1/1000,setpts=(N/25+if(gte(N\\,10)\\,0.02\\,0)+2)/TB[v];"
        "[1:a]asetpts=PTS+2.001/TB[a]",
        "-map", "[v]", "-map", "[a]", "-c:v", "ffv1", "-fps_mode", "vfr",
        "-enc_time_base:v", "1:1000",
        "-c:a", "pcm_f32le", "-copyts", "-avoid_negative_ts", "disabled",
        str(root / "sync.mkv"),
    ], check=True)
    subprocess.run([sys.argv[1], str(root / "sync.mkv")], check=True)
    subprocess.run([
        sys.argv[2], "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(root / "sync.mkv"), "-map", "0:v:0", "-map", "0:a:0",
        "-c:v", "mpeg4", "-c:a", "aac", "-b:a", "192k", "-copyts",
        "-fps_mode", "vfr", "-enc_time_base:v", "1:1000", "-avoid_negative_ts", "disabled",
        str(root / "sync.mp4"),
    ], check=True)
    subprocess.run([
        sys.argv[2], "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(root / "sync.mp4"), "-map", "0:a:0", "-f", "f32le",
        str(root / "aac-reference.f32"),
    ], check=True)
    subprocess.run([sys.argv[1], str(root / "sync.mp4"), "aac", str(root / "aac-reference.f32")], check=True)
    subprocess.run([
        sys.argv[2], "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "color=black:s=64x64:r=25:d=0.8",
        "-f", "f32le", "-ar", "48000", "-ac", "2", "-i", str(root / "audio.f32"),
        "-filter:a", "asetnsamples=n=1024:p=0,asetpts=PTS+0.185604/TB-"
        "(256*eq(mod(N/1024\\,3)\\,1)+128*eq(mod(N/1024\\,3)\\,2))",
        "-c:v", "ffv1", "-c:a", "pcm_f32le", str(root / "jitter.mkv"),
    ], check=True)
    subprocess.run([sys.argv[1], str(root / "jitter.mkv"), "jitter"], check=True)
    subprocess.run([
        sys.argv[2], "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "color=black:s=64x64:r=25:d=1.1",
        "-f", "f32le", "-ar", "48000", "-ac", "2", "-i", str(root / "audio.f32"),
        "-filter:a", "asetnsamples=n=480:p=0,asetpts=PTS+if(gte(N\\,24000)\\,0.1/TB\\,0)",
        "-c:v", "ffv1", "-c:a", "pcm_f32le", str(root / "gap.mkv"),
    ], check=True)
    subprocess.run([sys.argv[1], str(root / "gap.mkv"), "gap"], check=True)
