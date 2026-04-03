#pragma once

void getFilesystemStats(const char* path, unsigned long long* total, unsigned long long* free);
void updateGSSdFreeSpace();
void toggleGSRecording(int width, int height, const char* reason);
