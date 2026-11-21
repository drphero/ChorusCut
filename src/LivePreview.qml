import QtQuick
import QtMultimedia

// A take's live preview while it generates: the latest sampling step's clip, looping, silent.
// KJ Nodes sends an MP4 when the server can encode with NVENC (played with Qt's media player,
// which is given no audio output at all), otherwise an animated WebP. Each clip starts at the
// moment the clock says, so a new step's clip carries on where the last one was (the picture
// sharpens in place) and every copy (inspector tile, Jobs, viewer) shows the same moment, even
// when a list rebuilds its tiles. The previous clip stays up until the next one is playing, so a
// step never flashes black.
Item {
    id: lp
    property string source: ""
    property int fps: 24
    property int fillMode: Image.PreserveAspectFit
    property bool frontB: false
    property bool started: false
    readonly property bool ready: (frontB ? b : a).ready
    // The clip on screen (the latest step once it plays).
    readonly property string showing: ready ? (frontB ? b : a).src : ""

    function load(src) {
        const front = frontB ? b : a, back = frontB ? a : b
        if (!src) { a.src = ""; b.src = ""; return }
        if (front.src === src) return
        back.src = src
    }
    onSourceChanged: if (started) load(source)
    Component.onCompleted: { started = true; load(source) }

    component Slot: Item {
        id: slot
        property string src: ""
        property bool shown: false
        property bool ready: false
        readonly property bool isVideo: src.endsWith(".mp4")
        signal playing()
        anchors.fill: parent
        visible: shown && ready
        onSrcChanged: ready = false
        AnimatedImage {
            id: img
            anchors.fill: parent
            visible: !slot.isVideo
            source: slot.isVideo ? "" : slot.src
            fillMode: lp.fillMode
            cache: false       // a whole take's frames at preview size would be hundreds of MB
            smooth: true
            playing: lp.visible
            onStatusChanged: if (status === AnimatedImage.Ready) {
                currentFrame = frameCount > 1 ? Math.floor(Date.now() * lp.fps / 1000) % frameCount : 0
                slot.ready = true
                slot.playing()
            }
        }
        MediaPlayer {
            id: player
            source: slot.isVideo ? slot.src : ""
            videoOutput: out
            loops: MediaPlayer.Infinite
            // No audioOutput: these clips never make a sound.
            property bool started: false
            onSourceChanged: started = false
            onMediaStatusChanged: {
                if (started || (mediaStatus !== MediaPlayer.LoadedMedia && mediaStatus !== MediaPlayer.BufferedMedia)) return
                started = true
                if (duration > 0) position = Date.now() % duration
                if (lp.visible) play()
            }
            // Ready once a frame is moving, not merely loaded.
            onPositionChanged: if (started && !slot.ready && slot.isVideo) { slot.ready = true; slot.playing() }
        }
        VideoOutput {
            id: out
            anchors.fill: parent
            visible: slot.isVideo
            fillMode: lp.fillMode === Image.PreserveAspectCrop ? VideoOutput.PreserveAspectCrop : VideoOutput.PreserveAspectFit
        }
        Connections {
            target: lp
            function onVisibleChanged() {
                if (!slot.isVideo || !player.started) return
                if (lp.visible) { if (player.duration > 0) player.position = Date.now() % player.duration; player.play() }
                else player.pause()
            }
        }
    }
    Slot {
        id: a
        shown: !lp.frontB
        onPlaying: if (lp.frontB && src === lp.source) { lp.frontB = false; b.src = "" }
    }
    Slot {
        id: b
        shown: lp.frontB
        onPlaying: if (!lp.frontB && src === lp.source) { lp.frontB = true; a.src = "" }
    }
}
