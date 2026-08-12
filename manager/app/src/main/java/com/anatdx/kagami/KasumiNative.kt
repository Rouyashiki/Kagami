package com.anatdx.kagami

internal object KasumiNative {
    init {
        System.loadLibrary("kagami_client")
    }

    external fun statPath(path: String): String
}
