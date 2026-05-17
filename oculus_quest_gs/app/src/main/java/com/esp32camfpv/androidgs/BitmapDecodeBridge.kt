package com.esp32camfpv.androidgs

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import java.nio.ByteBuffer
import java.nio.ByteOrder

object BitmapDecodeBridge {
    class Result(
        val pixels: ByteBuffer,
        val width: Int,
        val height: Int,
        val stride: Int
    )

    @JvmStatic
    fun decodeRgb565(jpegData: ByteArray): Result? {
        return decode(jpegData, Bitmap.Config.RGB_565, 2)
    }

    @JvmStatic
    fun decodeRgb8888(jpegData: ByteArray): Result? {
        return decode(jpegData, Bitmap.Config.ARGB_8888, 4)
    }

    private fun decode(jpegData: ByteArray, config: Bitmap.Config, bytesPerPixel: Int): Result? {
        val options = BitmapFactory.Options().apply {
            inScaled = false
            inDither = false
            inPreferredConfig = config
        }
        val bitmap = BitmapFactory.decodeByteArray(jpegData, 0, jpegData.size, options) ?: return null
        return try {
            val sourceStride = bitmap.rowBytes
            val packedStride = bitmap.width * bytesPerPixel
            val sourceBuffer =
                ByteBuffer.allocateDirect(sourceStride * bitmap.height).order(ByteOrder.nativeOrder())
            bitmap.copyPixelsToBuffer(sourceBuffer)
            sourceBuffer.rewind()

            if (sourceStride == packedStride) {
                Result(sourceBuffer, bitmap.width, bitmap.height, packedStride)
            } else {
                val packedBuffer =
                    ByteBuffer.allocateDirect(packedStride * bitmap.height).order(ByteOrder.nativeOrder())
                repeat(bitmap.height) { row ->
                    val rowStart = row * sourceStride
                    sourceBuffer.position(rowStart)
                    sourceBuffer.limit(rowStart + packedStride)
                    packedBuffer.put(sourceBuffer)
                }
                packedBuffer.rewind()
                sourceBuffer.clear()
                Result(packedBuffer, bitmap.width, bitmap.height, packedStride)
            }
        } finally {
            bitmap.recycle()
        }
    }
}
