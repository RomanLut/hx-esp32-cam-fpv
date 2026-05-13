# Postprocessing

## Introduction

Postprocessing improves the final video image after decoding.  
It helps reduce visible artifacts and keeps the picture easier to watch, especially on highly compressed streams.

## JPEG Deblocking Filter

JPEG compression can leave visible "block" edges, especially in flat areas like sky or walls.  
The deblocking filter softens those hard edges so the image looks more natural.

- Reduces square/blocky patterns

- Best improvement on low-bitrate or noisy feeds
- Very small detail softening may appear in some scenes

## Adaptive Dithering

Adaptive dithering adds a subtle controlled noise pattern to reduce color banding.  
Banding is when smooth gradients (for example in shadows or sky) look like visible steps.

- Makes gradients look smoother
- Reduces visible color steps
