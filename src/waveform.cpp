#include "waveform.h"
#include "editor.h"
#include <QPainter>
#include <cmath>

WaveformView::WaveformView(QQuickItem* parent) : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    connect(this, &WaveformView::changed, this, [this] { update(); });
}

QObject* WaveformView::source() const { return editor.data(); }

void WaveformView::setSource(QObject* value) {
    auto next = qobject_cast<Editor*>(value);
    if (next == editor) return;
    if (editor) disconnect(editor, nullptr, this, nullptr);
    editor = next;
    if (editor) connect(editor, &Editor::peaksChanged, this, [this] { update(); });
    update();
    emit changed();
}

void WaveformView::paint(QPainter* p) {
    if (!editor || !editor->peaksReady() || pps <= 0) return;
    const auto& peaks = editor->peaks();
    const int buckets = peaks.size() / 2;
    const double bucketSeconds = Editor::kPeakMs / 1000.0;
    const double mid = height() / 2, half = height() / 2 - 1;
    p->setPen(QPen(color, 1));
    for (int x = 0; x < int(std::ceil(width())); ++x) {
        const double t0 = start + x / pps, t1 = start + (x + 1) / pps;
        int a = int(t0 / bucketSeconds), b = int(std::ceil(t1 / bucketSeconds));
        if (b <= 0 || a >= buckets) continue;
        a = qMax(0, a);
        b = qMin(buckets, qMax(a + 1, b));
        float lo = peaks[2 * a], hi = peaks[2 * a + 1];
        for (int i = a + 1; i < b; ++i) {
            lo = qMin(lo, peaks[2 * i]);
            hi = qMax(hi, peaks[2 * i + 1]);
        }
        p->drawLine(QPointF(x + 0.5, mid - hi * half), QPointF(x + 0.5, mid - lo * half + 0.01));
    }
}
