/********************************************************************
 KWin - the KDE window manager
 This file is part of the KDE project.

Copyright (C) 2013 Martin Gräßlin <mgraesslin@kde.org>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*********************************************************************/
#include "scene_qpainter.h"
// KWin
#include "client.h"
#include "composite.h"
#include "cursor.h"
#include "deleted.h"
#include "effects.h"
#include "main.h"
#include "screens.h"
#include "toplevel.h"
#if HAVE_WAYLAND
#include "abstract_backend.h"
#include "wayland_server.h"
#include <KWayland/Server/buffer_interface.h>
#include <KWayland/Server/surface_interface.h>
#endif
#include "xcbutils.h"
#include "decorations/decoratedclient.h"
// Qt
#include <QDebug>
#include <QPainter>
#include <KDecoration2/Decoration>

namespace KWin
{

//****************************************
// QPainterBackend
//****************************************
QPainterBackend::QPainterBackend()
    : m_failed(false)
{
}

QPainterBackend::~QPainterBackend()
{
}

OverlayWindow* QPainterBackend::overlayWindow()
{
    return NULL;
}

void QPainterBackend::showOverlay()
{
}

void QPainterBackend::screenGeometryChanged(const QSize &size)
{
    Q_UNUSED(size)
}

void QPainterBackend::setFailed(const QString &reason)
{
    qCWarning(KWIN_CORE) << "Creating the XRender backend failed: " << reason;
    m_failed = true;
}

void QPainterBackend::renderCursor(QPainter *painter)
{
    Q_UNUSED(painter)
}

bool QPainterBackend::perScreenRendering() const
{
    return false;
}

QImage *QPainterBackend::bufferForScreen(int screenId)
{
    Q_UNUSED(screenId)
    return buffer();
}

//****************************************
// SceneQPainter
//****************************************
SceneQPainter *SceneQPainter::createScene(QObject *parent)
{
    QScopedPointer<QPainterBackend> backend;
#if HAVE_WAYLAND
    if (kwinApp()->shouldUseWaylandForCompositing()) {
        backend.reset(waylandServer()->backend()->createQPainterBackend());
        if (backend.isNull()) {
            return nullptr;
        }
        if (backend->isFailed()) {
            return NULL;
        }
        return new SceneQPainter(backend.take(), parent);
    }
#endif
    return NULL;
}

SceneQPainter::SceneQPainter(QPainterBackend *backend, QObject *parent)
    : Scene(parent)
    , m_backend(backend)
    , m_painter(new QPainter())
{
}

SceneQPainter::~SceneQPainter()
{
}

CompositingType SceneQPainter::compositingType() const
{
    return QPainterCompositing;
}

bool SceneQPainter::initFailed() const
{
    return false;
}

void SceneQPainter::paintGenericScreen(int mask, ScreenPaintData data)
{
    m_painter->save();
    m_painter->translate(data.xTranslation(), data.yTranslation());
    m_painter->scale(data.xScale(), data.yScale());
    Scene::paintGenericScreen(mask, data);
    m_painter->restore();
}

qint64 SceneQPainter::paint(QRegion damage, ToplevelList toplevels)
{
    QElapsedTimer renderTimer;
    renderTimer.start();

    createStackingOrder(toplevels);

    int mask = 0;
    m_backend->prepareRenderingFrame();
    if (m_backend->perScreenRendering()) {
        const bool needsFullRepaint = m_backend->needsFullRepaint();
        if (needsFullRepaint) {
            mask |= Scene::PAINT_SCREEN_BACKGROUND_FIRST;
            damage = screens()->geometry();
        }
        QRegion overallUpdate;
        for (int i = 0; i < screens()->count(); ++i) {
            const QRect geometry = screens()->geometry(i);
            QImage *buffer = m_backend->bufferForScreen(i);
            if (!buffer || buffer->isNull()) {
                continue;
            }
            m_painter->begin(buffer);
            m_painter->save();
            m_painter->setWindow(geometry);

            QRegion updateRegion, validRegion;
            paintScreen(&mask, damage.intersected(geometry), QRegion(), &updateRegion, &validRegion);
            overallUpdate = overallUpdate.united(updateRegion);

            m_painter->restore();
            m_painter->end();
        }
        m_backend->showOverlay();
        m_backend->present(mask, overallUpdate);
    } else {
        m_painter->begin(m_backend->buffer());
        if (m_backend->needsFullRepaint()) {
            mask |= Scene::PAINT_SCREEN_BACKGROUND_FIRST;
            damage = QRegion(0, 0, displayWidth(), displayHeight());
        }
        QRegion updateRegion, validRegion;
        paintScreen(&mask, damage, QRegion(), &updateRegion, &validRegion);

        m_backend->renderCursor(m_painter.data());
        m_backend->showOverlay();

        m_painter->end();
        m_backend->present(mask, updateRegion);
    }

    // do cleanup
    clearStackingOrder();

    return renderTimer.nsecsElapsed();
}

void SceneQPainter::paintBackground(QRegion region)
{
    m_painter->setBrush(Qt::black);
    m_painter->drawRects(region.rects());
}

Scene::Window *SceneQPainter::createWindow(Toplevel *toplevel)
{
    return new SceneQPainter::Window(this, toplevel);
}

Scene::EffectFrame *SceneQPainter::createEffectFrame(EffectFrameImpl *frame)
{
    return new QPainterEffectFrame(frame, this);
}

Shadow *SceneQPainter::createShadow(Toplevel *toplevel)
{
    return new SceneQPainterShadow(toplevel);
}

void SceneQPainter::screenGeometryChanged(const QSize &size)
{
    Scene::screenGeometryChanged(size);
    m_backend->screenGeometryChanged(size);
}

//****************************************
// SceneQPainter::Window
//****************************************
SceneQPainter::Window::Window(SceneQPainter *scene, Toplevel *c)
    : Scene::Window(c)
    , m_scene(scene)
{
}

SceneQPainter::Window::~Window()
{
    discardShape();
}

void SceneQPainter::Window::performPaint(int mask, QRegion region, WindowPaintData data)
{
    if (!(mask & (PAINT_WINDOW_TRANSFORMED | PAINT_SCREEN_TRANSFORMED)))
        region &= toplevel->visibleRect();

    if (region.isEmpty())
        return;
    QPainterWindowPixmap *pixmap = windowPixmap<QPainterWindowPixmap>();
    if (!pixmap || !pixmap->isValid()) {
        return;
    }
    if (!toplevel->damage().isEmpty()) {
        pixmap->update(toplevel->damage());
        toplevel->resetDamage();
    }

    QPainter *scenePainter = m_scene->painter();
    QPainter *painter = scenePainter;
    painter->setClipRegion(region);
    painter->setClipping(true);

    painter->save();
    painter->translate(x(), y());
    if (mask & PAINT_WINDOW_TRANSFORMED) {
        painter->translate(data.xTranslation(), data.yTranslation());
        painter->scale(data.xScale(), data.yScale());
    }

    const bool opaque = qFuzzyCompare(1.0, data.opacity());
    QImage tempImage;
    QPainter tempPainter;
    if (!opaque) {
        // need a temp render target which we later on blit to the screen
        tempImage = QImage(toplevel->visibleRect().size(), QImage::Format_ARGB32_Premultiplied);
        tempImage.fill(Qt::transparent);
        tempPainter.begin(&tempImage);
        tempPainter.save();
        tempPainter.translate(toplevel->geometry().topLeft() - toplevel->visibleRect().topLeft());
        painter = &tempPainter;
    }
    renderShadow(painter);
    renderWindowDecorations(painter);

    // render content
    const QRect src = QRect(toplevel->clientPos(), toplevel->clientSize());
    painter->drawImage(toplevel->clientPos(), pixmap->image(), src);

    if (!opaque) {
        tempPainter.restore();
        tempPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QColor translucent(Qt::transparent);
        translucent.setAlphaF(data.opacity());
        tempPainter.fillRect(QRect(QPoint(0, 0), toplevel->visibleRect().size()), translucent);
        tempPainter.end();
        painter = scenePainter;
        painter->drawImage(toplevel->visibleRect().topLeft() - toplevel->geometry().topLeft(), tempImage);
    }

    painter->restore();

    painter->setClipRegion(QRegion());
    painter->setClipping(false);
}

void SceneQPainter::Window::renderShadow(QPainter* painter)
{
    if (!toplevel->shadow()) {
        return;
    }
    SceneQPainterShadow *shadow = static_cast<SceneQPainterShadow *>(toplevel->shadow());
    const QPixmap &topLeft     = shadow->shadowPixmap(SceneQPainterShadow::ShadowElementTopLeft);
    const QPixmap &top         = shadow->shadowPixmap(SceneQPainterShadow::ShadowElementTop);
    const QPixmap &topRight    = shadow->shadowPixmap(SceneQPainterShadow::ShadowElementTopRight);
    const QPixmap &bottomLeft  = shadow->shadowPixmap(SceneQPainterShadow::ShadowElementBottomLeft);
    const QPixmap &bottom      = shadow->shadowPixmap(SceneQPainterShadow::ShadowElementBottom);
    const QPixmap &bottomRight = shadow->shadowPixmap(SceneQPainterShadow::ShadowElementBottomRight);
    const QPixmap &left        = shadow->shadowPixmap(SceneQPainterShadow::ShadowElementLeft);
    const QPixmap &right       = shadow->shadowPixmap(SceneQPainterShadow::ShadowElementRight);

    const int leftOffset   = shadow->leftOffset();
    const int topOffset    = shadow->topOffset();
    const int rightOffset  = shadow->rightOffset();
    const int bottomOffset = shadow->bottomOffset();

    // top left
    painter->drawPixmap(-leftOffset, -topOffset, topLeft);
    // top right
    painter->drawPixmap(toplevel->width() - topRight.width() + rightOffset, -topOffset, topRight);
    // bottom left
    painter->drawPixmap(-leftOffset, toplevel->height() - bottomLeft.height() + bottomOffset, bottomLeft);
    // bottom right
    painter->drawPixmap(toplevel->width() - bottomRight.width() + rightOffset,
                        toplevel->height() - bottomRight.height() + bottomOffset,
                        bottomRight);
    // top
    painter->drawPixmap(topLeft.width() - leftOffset, -topOffset,
                        toplevel->width() - topLeft.width() - topRight.width() + leftOffset + rightOffset,
                        top.height(),
                        top);
    // left
    painter->drawPixmap(-leftOffset, topLeft.height() - topOffset, left.width(),
                        toplevel->height() - topLeft.height() - bottomLeft.height() + topOffset + bottomOffset,
                        left);
    // right
    painter->drawPixmap(toplevel->width() - right.width() + rightOffset,
                        topRight.height() - topOffset,
                        right.width(),
                        toplevel->height() - topRight.height() - bottomRight.height() + topOffset + bottomOffset,
                        right);
    // bottom
    painter->drawPixmap(bottomLeft.width() - leftOffset,
                        toplevel->height() - bottom.height() + bottomOffset,
                        toplevel->width() - bottomLeft.width() - bottomRight.width() + leftOffset + rightOffset,
                        bottom.height(),
                        bottom);
}

void SceneQPainter::Window::renderWindowDecorations(QPainter *painter)
{
    // TODO: custom decoration opacity
    Client *client = dynamic_cast<Client*>(toplevel);
    Deleted *deleted = dynamic_cast<Deleted*>(toplevel);
    if (!client && !deleted) {
        return;
    }

    bool noBorder = true;
    const SceneQPainterDecorationRenderer *renderer = nullptr;
    QRect dtr, dlr, drr, dbr;
    if (client && !client->noBorder()) {
        if (client->isDecorated()) {
            if (SceneQPainterDecorationRenderer *r = static_cast<SceneQPainterDecorationRenderer *>(client->decoratedClient()->renderer())) {
                r->render();
                renderer = r;
            }
        }
        client->layoutDecorationRects(dlr, dtr, drr, dbr);
        noBorder = false;
    } else if (deleted && !deleted->noBorder()) {
        noBorder = false;
        deleted->layoutDecorationRects(dlr, dtr, drr, dbr);
        renderer = static_cast<const SceneQPainterDecorationRenderer *>(deleted->decorationRenderer());
    }
    if (noBorder || !renderer) {
        return;
    }

    painter->drawImage(dtr, renderer->image(SceneQPainterDecorationRenderer::DecorationPart::Top));
    painter->drawImage(dlr, renderer->image(SceneQPainterDecorationRenderer::DecorationPart::Left));
    painter->drawImage(drr, renderer->image(SceneQPainterDecorationRenderer::DecorationPart::Right));
    painter->drawImage(dbr, renderer->image(SceneQPainterDecorationRenderer::DecorationPart::Bottom));
}

WindowPixmap *SceneQPainter::Window::createWindowPixmap()
{
    return new QPainterWindowPixmap(this);
}

Decoration::Renderer *SceneQPainter::createDecorationRenderer(Decoration::DecoratedClientImpl *impl)
{
    return new SceneQPainterDecorationRenderer(impl);
}

//****************************************
// QPainterWindowPixmap
//****************************************
QPainterWindowPixmap::QPainterWindowPixmap(Scene::Window *window)
    : WindowPixmap(window)
    , m_shm(kwinApp()->shouldUseWaylandForCompositing() ? nullptr : new Xcb::Shm)
{
}

QPainterWindowPixmap::~QPainterWindowPixmap()
{
}

void QPainterWindowPixmap::create()
{
    if (isValid()) {
        return;
    }
    if (!kwinApp()->shouldUseWaylandForCompositing() && !m_shm->isValid()) {
        return;
    }
    KWin::WindowPixmap::create();
    if (!isValid()) {
        return;
    }
#if HAVE_WAYLAND
    if (kwinApp()->shouldUseWaylandForCompositing()) {
        // performing deep copy, this could probably be improved
        m_image = buffer()->data().copy();
        return;
    }
#endif
    m_image = QImage((uchar*)m_shm->buffer(), size().width(), size().height(), QImage::Format_ARGB32_Premultiplied);
}

bool QPainterWindowPixmap::update(const QRegion &damage)
{
#if HAVE_WAYLAND
    if (kwinApp()->shouldUseWaylandForCompositing()) {
        const auto oldBuffer = buffer();
        updateBuffer();
        const auto &b = buffer();
        if (b == oldBuffer || b.isNull()) {
            return false;
        }
        QPainter p(&m_image);
        const QImage &data = b->data();
        p.setCompositionMode(QPainter::CompositionMode_Source);
        for (const QRect &rect : damage.rects()) {
            p.drawImage(rect, data, rect);
        }
        return true;
    }
#endif

    if (!m_shm->isValid()) {
        return false;
    }

    // TODO: optimize by only updating the damaged areas
    xcb_shm_get_image_cookie_t cookie = xcb_shm_get_image_unchecked(connection(), pixmap(),
        0, 0, size().width(), size().height(),
        ~0, XCB_IMAGE_FORMAT_Z_PIXMAP, m_shm->segment(), 0);

    ScopedCPointer<xcb_shm_get_image_reply_t> image(xcb_shm_get_image_reply(connection(), cookie, NULL));
    if (image.isNull()) {
        return false;
    }
    return true;
}

QPainterEffectFrame::QPainterEffectFrame(EffectFrameImpl *frame, SceneQPainter *scene)
    : Scene::EffectFrame(frame)
    , m_scene(scene)
{
}

QPainterEffectFrame::~QPainterEffectFrame()
{
}

void QPainterEffectFrame::render(QRegion region, double opacity, double frameOpacity)
{
    Q_UNUSED(region)
    Q_UNUSED(opacity)
    // TODO: adjust opacity
    if (m_effectFrame->geometry().isEmpty()) {
        return; // Nothing to display
    }
    QPainter *painter = m_scene->painter();


    // Render the actual frame
    if (m_effectFrame->style() == EffectFrameUnstyled) {
        painter->save();
        painter->setPen(Qt::NoPen);
        QColor color(Qt::black);
        color.setAlphaF(frameOpacity);
        painter->setBrush(color);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->drawRoundedRect(m_effectFrame->geometry().adjusted(-5, -5, 5, 5), 5.0, 5.0);
        painter->restore();
    } else if (m_effectFrame->style() == EffectFrameStyled) {
        qreal left, top, right, bottom;
        m_effectFrame->frame().getMargins(left, top, right, bottom);   // m_geometry is the inner geometry
        QRect geom = m_effectFrame->geometry().adjusted(-left, -top, right, bottom);
        painter->drawPixmap(geom, m_effectFrame->frame().framePixmap());
    }
    if (!m_effectFrame->selection().isNull()) {
        painter->drawPixmap(m_effectFrame->selection(), m_effectFrame->selectionFrame().framePixmap());
    }

    // Render icon
    if (!m_effectFrame->icon().isNull() && !m_effectFrame->iconSize().isEmpty()) {
        const QPoint topLeft(m_effectFrame->geometry().x(),
                             m_effectFrame->geometry().center().y() - m_effectFrame->iconSize().height() / 2);

        const QRect geom = QRect(topLeft, m_effectFrame->iconSize());
        painter->drawPixmap(geom, m_effectFrame->icon().pixmap(m_effectFrame->iconSize()));
    }

    // Render text
    if (!m_effectFrame->text().isEmpty()) {
        // Determine position on texture to paint text
        QRect rect(QPoint(0, 0), m_effectFrame->geometry().size());
        if (!m_effectFrame->icon().isNull() && !m_effectFrame->iconSize().isEmpty()) {
            rect.setLeft(m_effectFrame->iconSize().width());
        }

        // If static size elide text as required
        QString text = m_effectFrame->text();
        if (m_effectFrame->isStatic()) {
            QFontMetrics metrics(m_effectFrame->text());
            text = metrics.elidedText(text, Qt::ElideRight, rect.width());
        }

        painter->save();
        painter->setFont(m_effectFrame->font());
        if (m_effectFrame->style() == EffectFrameStyled) {
            painter->setPen(m_effectFrame->styledTextColor());
        } else {
            // TODO: What about no frame? Custom color setting required
            painter->setPen(Qt::white);
        }
        painter->drawText(rect.translated(m_effectFrame->geometry().topLeft()), m_effectFrame->alignment(), text);
        painter->restore();
    }
}

//****************************************
// QPainterShadow
//****************************************
SceneQPainterShadow::SceneQPainterShadow(Toplevel* toplevel)
    : Shadow(toplevel)
{
}

SceneQPainterShadow::~SceneQPainterShadow()
{
}

bool SceneQPainterShadow::prepareBackend()
{
    if (hasDecorationShadow()) {
        // TODO: implement for QPainter
        return false;
    }
    return true;
}

//****************************************
// QPainterDecorationRenderer
//****************************************
SceneQPainterDecorationRenderer::SceneQPainterDecorationRenderer(Decoration::DecoratedClientImpl *client)
    : Renderer(client)
{
    connect(this, &Renderer::renderScheduled, client->client(), static_cast<void (Client::*)(const QRect&)>(&Client::addRepaint));
}

SceneQPainterDecorationRenderer::~SceneQPainterDecorationRenderer() = default;

QImage SceneQPainterDecorationRenderer::image(SceneQPainterDecorationRenderer::DecorationPart part) const
{
    Q_ASSERT(part != DecorationPart::Count);
    return m_images[int(part)];
}

void SceneQPainterDecorationRenderer::render()
{
    const QRegion scheduled = getScheduled();
    if (scheduled.isEmpty()) {
        return;
    }
    if (areImageSizesDirty()) {
        resizeImages();
        resetImageSizesDirty();
    }

    const QRect top(QPoint(0, 0), m_images[int(DecorationPart::Top)].size());
    const QRect left(QPoint(0, top.height()), m_images[int(DecorationPart::Left)].size());
    const QRect right(QPoint(top.width() - m_images[int(DecorationPart::Right)].size().width(), top.height()), m_images[int(DecorationPart::Right)].size());
    const QRect bottom(QPoint(0, left.y() + left.height()), m_images[int(DecorationPart::Bottom)].size());

    const QRect geometry = scheduled.boundingRect();
    auto renderPart = [this](const QRect &rect, const QRect &partRect, int index) {
        if (rect.isEmpty()) {
            return;
        }
        QPainter painter(&m_images[index]);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setWindow(partRect);
        painter.setClipRect(rect);
        painter.save();
        // clear existing part
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect, Qt::transparent);
        painter.restore();
        client()->decoration()->paint(&painter, rect);
    };

    renderPart(left.intersected(geometry), left, int(DecorationPart::Left));
    renderPart(top.intersected(geometry), top, int(DecorationPart::Top));
    renderPart(right.intersected(geometry), right, int(DecorationPart::Right));
    renderPart(bottom.intersected(geometry), bottom, int(DecorationPart::Bottom));
}

void SceneQPainterDecorationRenderer::resizeImages()
{
    QRect left, top, right, bottom;
    client()->client()->layoutDecorationRects(left, top, right, bottom);

    auto checkAndCreate = [this](int index, const QSize &size) {
        if (m_images[index].size() != size) {
            m_images[index] = QImage(size, QImage::Format_ARGB32_Premultiplied);
            m_images[index].fill(Qt::transparent);
        }
    };
    checkAndCreate(int(DecorationPart::Left), left.size());
    checkAndCreate(int(DecorationPart::Right), right.size());
    checkAndCreate(int(DecorationPart::Top), top.size());
    checkAndCreate(int(DecorationPart::Bottom), bottom.size());
}

void SceneQPainterDecorationRenderer::reparent(Deleted *deleted)
{
    render();
    Renderer::reparent(deleted);
}

} // KWin
