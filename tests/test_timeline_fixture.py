"""MOV timing tables vs decoded B-frames, VFR, edit lists and ProRes."""
import json
import pathlib
import subprocess
import sys
import tempfile

ffmpeg, ffprobe = sys.argv[2:4]
with tempfile.TemporaryDirectory(prefix="hdrplay-box-timeline-") as directory:
    root = pathlib.Path(directory)
    for codec, extension in [("mpeg4", "mp4"), ("prores_ks", "mov")]:
        movie = root / f"timeline.{extension}"
        video_options = (["-q:v", "3", "-g", "25", "-bf", "2"]
                         if codec == "mpeg4" else ["-profile:v", "0", "-pix_fmt", "yuv422p10le"])
        subprocess.run([
            ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi", "-i", "testsrc2=s=64x64:r=25:d=12",
            "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo:d=12.1",
            "-filter_complex",
            "[0:v]settb=1/1000,setpts=(N/25+if(gte(N\\,150)\\,0.04\\,0)+2)/TB[v];"
            "[1:a]asetpts=PTS+2.001/TB[a]",
            "-map", "[v]", "-map", "[a]", "-c:v", codec, *video_options,
            "-fps_mode", "vfr", "-enc_time_base:v", "1:1000",
            "-c:a", "aac", "-copyts", "-avoid_negative_ts", "disabled", str(movie),
        ], check=True)
        probe = json.loads(subprocess.check_output([
            ffprobe, "-v", "error", "-select_streams", "v:0", "-show_frames",
            "-show_entries", "frame=best_effort_timestamp,duration,pict_type,key_frame", "-of", "json", str(movie),
        ]))
        if codec == "mpeg4":
            assert any(frame["pict_type"] == "B" for frame in probe["frames"])
        reference = root / "reference.txt"
        reference.write_text("\n".join(f'{f["best_effort_timestamp"]} {f.get("duration",0)} {f["key_frame"]}' for f in probe["frames"]))
        subprocess.run([sys.argv[1], str(movie), str(reference)], check=True)
