# PFFFT (vendored)

Pretty Fast FFT by Julien Pommier, used by `ptutil/Filt/FiltPartConv.cpp` for
FFT partitioned convolution in the linear-phase brickwall filter.

- Source: https://bitbucket.org/jpommier/pffft
- Commit: 0aec0327a6912e1a0ec5326eef737c2ce19bc836
- Files: `pffft.c`, `pffft.h`, unmodified.
- Licence: FFTPACK/BSD-style (see the header of `pffft.c`), compatible with
  FxSound's AGPL-3.0.

`pffft.c` is compiled with optimisation even in Debug configurations (see
`DfxDsp.vcxproj`), because an unoptimised FFT is too slow for real-time audio.
