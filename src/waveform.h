#pragma once
#include <QColor>
#include <QPointer>
#include <QtQuick/QQuickPaintedItem>

class Editor;

// Draws the master recording's waveform for the visible part of the timeline.
// The timeline owns zoom (pixels per second) and scroll (seconds at the left edge).
class WaveformView : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QObject* source READ source WRITE setSource NOTIFY changed)
    Q_PROPERTY(double pixelsPerSecond MEMBER pps WRITE setPps NOTIFY changed)
    Q_PROPERTY(double viewStart MEMBER start WRITE setStart NOTIFY changed)
    Q_PROPERTY(QColor color MEMBER color NOTIFY changed)
public:
    explicit WaveformView(QQuickItem* parent = nullptr);
    QObject* source() const;
    void setSource(QObject* value);
    void setPps(double value) { if (value != pps) { pps = value; update(); emit changed(); } }
    void setStart(double value) { if (value != start) { start = value; update(); emit changed(); } }
    void paint(QPainter* painter) override;
signals:
    void changed();
private:
    QPointer<Editor> editor;
    double pps = 20, start = 0;
    QColor color = QColor("#5fae96");
};
