package com.swordfish.libretrodroid

/**
 * Lockstep netplay with one other device running the same content.
 *
 * @param socketFd a connected TCP socket, detached from its owner: LibretroDroid closes it.
 * @param isHost the host's state is the one both devices start from.
 * @param localPort the port this device's controller drives. The other device drives the other one of 0 and 1.
 * @param inputDelay frames between pressing a button and the game seeing it, to hide the network latency.
 */
data class NetplayConfig(
    val socketFd: Int,
    val isHost: Boolean,
    val localPort: Int,
    val inputDelay: Int,
)
