# Source video for the `ffmpeg` workload

`noise_160x90.h264` is 2 seconds (48 frames, 24 fps, 160x90, 4:2:0) of random
noise, a raw H.264 (Annex B) stream. Each frame is independent noise, so
neither intra nor inter prediction helps much. The resolution is low because
noise does not compress: the same clip at 640x360 is about 3 MB.

Made with FFmpeg 6.1.1:

```sh
ffmpeg -f lavfi -i "color=c=gray:s=160x90:r=24,noise=alls=100:allf=t+u:all_seed=12345,format=yuv420p" \
  -frames:v 48 -c:v libx264 -preset veryslow -crf 30 -g 48 -bf 2 \
  -x264-params no-info=1 -f h264 noise_160x90.h264
```
