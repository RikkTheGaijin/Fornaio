#pragma once
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QWheelEvent>
#include <QApplication>
#include <QAbstractScrollArea>

// Let the surrounding scroll area handle the wheel. Values are changed explicitly
// by typing, clicking arrows, selecting the dropdown, or using keyboard navigation.
template <class Base> class ScrollSafe : public Base {
  public:
    using Base::Base;

  protected:
    void wheelEvent(QWheelEvent *event) override {
        // Explicit forwarding also covers focused editors and Qt's native wheel routing,
        // where simply ignoring the event does not reliably scroll the containing panel.
        for (QWidget *parent = this->parentWidget(); parent; parent = parent->parentWidget()) {
            if (auto area = qobject_cast<QAbstractScrollArea *>(parent)) {
                QWheelEvent forwarded(area->viewport()->mapFromGlobal(event->globalPosition().toPoint()),
                                      event->globalPosition(), event->pixelDelta(), event->angleDelta(),
                                      event->buttons(), event->modifiers(), event->phase(), event->inverted());
                QApplication::sendEvent(area->viewport(), &forwarded);
                event->accept();
                return;
            }
        }
        event->ignore();
    }
};
using ScrollComboBox = ScrollSafe<QComboBox>;
using ScrollSpinBox = ScrollSafe<QSpinBox>;
using ScrollDoubleSpinBox = ScrollSafe<QDoubleSpinBox>;
