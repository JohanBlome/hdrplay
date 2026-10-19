"""Exercise offset playback with a decoded fixture and CLI validation."""
import pathlib
import os
import subprocess
import sys
import tempfile

source_test, hdrplay, ffmpeg = sys.argv[1:4]
help_text = None
for flag in ["-h", "--help"]:
    result = subprocess.run([hdrplay, flag], capture_output=True, text=True)
    assert result.returncode == 0 and not result.stderr, result
    for section in ["PLAYBACK", "COMPARISON", "IMAGE VIEW", "SCOPES AND INSPECTION",
                    "AUDIO WAVEFORM", "AUDIO MARKERS", "TIMING MARKERS"]:
        assert section in result.stdout, section
    assert "--offset" in result.stdout and "Area: Video window" in result.stdout
    if help_text is not None:
        assert result.stdout == help_text
    help_text = result.stdout

with tempfile.TemporaryDirectory(prefix="hdrplay-offset-") as directory:
    movie = pathlib.Path(directory) / "offset.mp4"
    subprocess.run([
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi", "-i", "testsrc2=s=64x64:r=10:d=5",
        "-c:v", "mpeg4", "-g", "10", "-bf", "2", str(movie),
    ], check=True)
    subprocess.run([source_test, str(movie)], check=True)
    for value in ["nan", "inf", "1e99", "2:1", "1:", "0.5oops", "",
                  "0.5f", "nanf", "5ff", "5frames", "fs"]:
        result = subprocess.run([hdrplay, "--offset", value, str(movie), str(movie)],
                                capture_output=True, text=True)
        assert result.returncode == 2 and "--offset" in result.stderr, (value, result)
    for value in ["5f", "-5f", "0:3f", "1:-3f", "0f", "0.5s", "0.5"]:
        # Help exits before display setup; the preceding operand is parsed.
        result = subprocess.run([hdrplay, "--offset", value, "-h"],
                                capture_output=True, text=True)
        assert result.returncode == 0 and not result.stderr, (value, result)
    # The dummy video driver cannot create the Vulkan player window, but
    # startup resolves and logs every input offset before creating it.
    environment = dict(os.environ, SDL_VIDEO_DRIVER="dummy")
    for options, paths, expected in [
        (["--offset", "5f"], [str(movie)], ["input 0 video offset +5f"]),
        (["--offset", "5f"], [str(movie), str(movie)],
         ["input 0 video offset +5f", "input 1 video offset +5f"]),
        (["--offset", "1:5f"], [str(movie), str(movie)],
         ["input 1 video offset +5f"]),
        (["--offset", "0.5s", "--offset", "1:-3f"], [str(movie), str(movie)],
         ["input 0 video offset +0.500000s", "input 1 video offset -3f"]),
        (["--offset", "1:-3f", "--offset", "5f"], [str(movie), str(movie)],
         ["input 0 video offset +5f", "input 1 video offset +5f"]),
    ]:
        result = subprocess.run([hdrplay, *options, *paths], env=environment,
                                capture_output=True, text=True, timeout=15)
        for message in expected:
            assert message in result.stderr, (options, result)
        assert result.stderr.count("video offset") == len(expected), result
    for args in [["--offset", "1:0.5", str(movie)],
                 ["--analyze", "--offset", "0:0.5", str(movie)]]:
        result = subprocess.run([hdrplay, *args], capture_output=True, text=True)
        assert result.returncode == 2 and "--offset" in result.stderr, result
