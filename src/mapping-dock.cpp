/*
OBS Projection Mapping
Copyright (C) 2026 Matias Knudsen <matiasknuds@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/*
 * Panelet "Projection Mapping": en fast dock i OBS-vinduet som viser
 * mappingen og lar deg dra hjørner/kanter/flater direkte med musa.
 * Museklikk sendes videre til kilden (samme logikk som «Samhandle»).
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>
#include <atomic>
#include <string>

#define PM_ID "projection_mapping_source"

namespace {

/* ------------------------------------------------------------------ */
/* Visning med egen OBS-display + museinteraksjon                       */

class MappingView : public QWidget {
public:
	explicit MappingView(QWidget *parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMouseTracking(true);
		setFocusPolicy(Qt::StrongFocus);
		setMinimumSize(240, 140);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	}

	~MappingView() override
	{
		destroyDisplay();
		obs_weak_source_release(weak);
	}

	QPaintEngine *paintEngine() const override { return nullptr; }

	void setSource(obs_source_t *source)
	{
		obs_weak_source_release(weak);
		weak = source ? obs_source_get_weak_source(source) : nullptr;
	}

	obs_source_t *source() const { return weak ? obs_weak_source_get_source(weak) : nullptr; }

	std::function<void()> onRemoveKey;

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		createDisplay();
	}

	void paintEvent(QPaintEvent *) override { createDisplay(); }

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		createDisplay();
		if (display) {
			QSize s = pixelSize();
			obs_display_resize(display, (uint32_t)s.width(), (uint32_t)s.height());
		}
	}

	bool event(QEvent *e) override
	{
		if (e->type() == QEvent::PlatformSurface) {
			auto *pe = static_cast<QPlatformSurfaceEvent *>(e);
			if (pe->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
				destroyDisplay();
		} else if (e->type() == QEvent::Expose || e->type() == QEvent::WinIdChange) {
			createDisplay();
		}
		return QWidget::event(e);
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		setFocus();
		sendClick(e, false);
	}
	void mouseReleaseEvent(QMouseEvent *e) override { sendClick(e, true); }

	void mouseMoveEvent(QMouseEvent *e) override
	{
		obs_source_t *src = source();
		if (!src)
			return;
		obs_mouse_event ev = toObs(e->position());
		obs_source_send_mouse_move(src, &ev, false);
		obs_source_release(src);
	}

	void keyPressEvent(QKeyEvent *e) override
	{
		if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && onRemoveKey) {
			onRemoveKey();
			return;
		}
		QWidget::keyPressEvent(e);
	}

private:
	obs_display_t *display = nullptr;
	obs_weak_source_t *weak = nullptr;

	/* Skrives av tegnetråden, leses av UI-tråden ved museklikk. */
	std::atomic<float> scale{1.0f};
	std::atomic<float> offsetX{0.0f};
	std::atomic<float> offsetY{0.0f};

	QSize pixelSize() const
	{
		qreal dpr = devicePixelRatioF();
		return QSize(std::max(1, (int)(width() * dpr)), std::max(1, (int)(height() * dpr)));
	}

	obs_mouse_event toObs(const QPointF &pos) const
	{
		qreal dpr = devicePixelRatioF();
		float s = scale.load();
		if (s <= 0.0f)
			s = 1.0f;
		obs_mouse_event ev = {};
		ev.x = (int32_t)(((float)(pos.x() * dpr) - offsetX.load()) / s);
		ev.y = (int32_t)(((float)(pos.y() * dpr) - offsetY.load()) / s);
		return ev;
	}

	void sendClick(QMouseEvent *e, bool up)
	{
		if (e->button() != Qt::LeftButton)
			return;
		obs_source_t *src = source();
		if (!src)
			return;
		obs_mouse_event ev = toObs(e->position());
		obs_source_send_mouse_click(src, &ev, MOUSE_LEFT, up, 1);
		obs_source_release(src);
	}

	bool fillWindow(gs_window &w)
	{
		QWindow *win = windowHandle();
		if (!win)
			return false;
#ifdef _WIN32
		w.hwnd = (HWND)win->winId();
#elif defined(__APPLE__)
		w.view = (id)win->winId();
#else
		if (obs_get_nix_platform() != OBS_NIX_PLATFORM_X11_EGL)
			return false; /* Wayland støttes ikke i panelet ennå */
		w.id = (uint32_t)win->winId();
		w.display = obs_get_nix_platform_display();
#endif
		return true;
	}

	void createDisplay()
	{
		if (display || !isVisible() || width() <= 0 || height() <= 0)
			return;

		gs_init_data info = {};
		if (!fillWindow(info.window))
			return;
		QSize s = pixelSize();
		info.cx = (uint32_t)s.width();
		info.cy = (uint32_t)s.height();
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;

		display = obs_display_create(&info, 0x1e1e1e);
		if (display)
			obs_display_add_draw_callback(display, &MappingView::draw, this);
	}

	void destroyDisplay()
	{
		if (!display)
			return;
		obs_display_remove_draw_callback(display, &MappingView::draw, this);
		obs_display_destroy(display);
		display = nullptr;
	}

	static void draw(void *param, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<MappingView *>(param);
		obs_source_t *src = self->source();
		if (!src)
			return;

		uint32_t sw = obs_source_get_width(src);
		uint32_t sh = obs_source_get_height(src);
		if (sw && sh && cx && cy) {
			float s = std::min((float)cx / (float)sw, (float)cy / (float)sh);
			int vw = (int)((float)sw * s), vh = (int)((float)sh * s);
			int ox = ((int)cx - vw) / 2, oy = ((int)cy - vh) / 2;
			self->scale.store(s);
			self->offsetX.store((float)ox);
			self->offsetY.store((float)oy);

			gs_viewport_push();
			gs_projection_push();
			gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
			gs_set_viewport(ox, oy, vw, vh);
			obs_source_video_render(src);
			gs_projection_pop();
			gs_viewport_pop();
		}
		obs_source_release(src);
	}
};

/* ------------------------------------------------------------------ */
/* Selve panelet                                                        */

class MappingDock : public QWidget {
public:
	explicit MappingDock(QWidget *parent) : QWidget(parent)
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(6, 6, 6, 6);
		root->setSpacing(6);

		sourceCombo = new QComboBox(this);
		sourceCombo->setToolTip("Hvilken Projection Mapping-kilde panelet redigerer");
		root->addWidget(sourceCombo);

		auto *row1 = new QHBoxLayout();
		auto *addQuad = new QPushButton("+ Firkant", this);
		auto *addRound = new QPushButton("+ Rund", this);
		auto *toggle = new QPushButton("Rund ⇄ firkant", this);
		auto *remove = new QPushButton("Fjern", this);
		addQuad->setToolTip("Legg til en ny firkantet flate");
		addRound->setToolTip("Legg til en ny rund flate");
		toggle->setToolTip("Gjør valgt flate rund eller firkantet");
		remove->setToolTip("Fjern valgt flate (eller trykk Delete)");
		row1->addWidget(addQuad);
		row1->addWidget(addRound);
		row1->addWidget(toggle);
		row1->addWidget(remove);
		root->addLayout(row1);

		auto *row2 = new QHBoxLayout();
		modeGroup = new QButtonGroup(this);
		modeGroup->setExclusive(true);
		const char *names[] = {"Flater", "Utsnitt", "Ferdig"};
		const int modes[] = {1, 2, 0};
		const char *tips[] = {"Dra hjørner, kanter og flater på projektorbildet",
				      "Velg hvilken del av innholdet hver flate viser",
				      "Skjul hjelpelinjene (til visning)"};
		for (int i = 0; i < 3; i++) {
			auto *b = new QPushButton(names[i], this);
			b->setCheckable(true);
			b->setToolTip(tips[i]);
			modeGroup->addButton(b, modes[i]);
			row2->addWidget(b);
		}
		auto *reset = new QPushButton("Nullstill", this);
		reset->setToolTip("Sett valgt flate tilbake til start");
		row2->addWidget(reset);
		root->addLayout(row2);

		view = new MappingView(this);
		root->addWidget(view, 1);

		hint = new QLabel(this);
		hint->setWordWrap(true);
		hint->setStyleSheet("color: #9a9a9a;");
		root->addWidget(hint);

		connect(addQuad, &QPushButton::clicked, this, [this]() { addSurface(0); });
		connect(addRound, &QPushButton::clicked, this, [this]() { addSurface(1); });
		connect(toggle, &QPushButton::clicked, this, [this]() { callSimple("pm_toggle_shape"); });
		connect(remove, &QPushButton::clicked, this, [this]() { callSimple("pm_remove_surface"); });
		connect(reset, &QPushButton::clicked, this, [this]() { callSimple("pm_reset_surface"); });
		connect(modeGroup, &QButtonGroup::idClicked, this, [this](int mode) { setMode(mode); });
		connect(sourceCombo, &QComboBox::currentIndexChanged, this, [this](int) { selectCurrent(); });
		view->onRemoveKey = [this]() {
			callSimple("pm_remove_surface");
		};

		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start(1000);
		refresh();
	}

private:
	QComboBox *sourceCombo;
	QButtonGroup *modeGroup;
	MappingView *view;
	QLabel *hint;
	QTimer *timer;
	std::string currentUuid;

	static bool collect(void *param, obs_source_t *src)
	{
		auto *list = static_cast<std::vector<std::pair<std::string, std::string>> *>(param);
		if (strcmp(obs_source_get_unversioned_id(src), PM_ID) == 0)
			list->emplace_back(obs_source_get_name(src), obs_source_get_uuid(src));
		return true;
	}

	/* Oppdaterer listen over mapping-kilder og modusknappene. */
	void refresh()
	{
		std::vector<std::pair<std::string, std::string>> list;
		obs_enum_sources(collect, &list);

		bool changed = (int)list.size() != sourceCombo->count();
		for (int i = 0; !changed && i < (int)list.size(); i++) {
			if (sourceCombo->itemData(i).toString().toStdString() != list[i].second ||
			    sourceCombo->itemText(i).toStdString() != list[i].first)
				changed = true;
		}

		if (changed) {
			QSignalBlocker block(sourceCombo);
			sourceCombo->clear();
			int keep = -1;
			for (int i = 0; i < (int)list.size(); i++) {
				sourceCombo->addItem(QString::fromStdString(list[i].first),
						     QString::fromStdString(list[i].second));
				if (list[i].second == currentUuid)
					keep = i;
			}
			if (keep < 0 && !list.empty())
				keep = 0;
			sourceCombo->setCurrentIndex(keep);
			selectCurrent();
		}

		bool have = !list.empty();
		setControlsEnabled(have);
		if (!have) {
			hint->setText("Legg til kilden «Projection Mapping» i en scene (trykk + under Kilder), "
				      "så dukker den opp her.");
		} else {
			hint->setText("Dra hjørner for perspektiv, kantene for å flytte en side, "
				      "midt i flaten for å flytte hele. Delete fjerner valgt flate.");
			syncMode();
		}
	}

	void setControlsEnabled(bool on)
	{
		for (auto *b : findChildren<QPushButton *>())
			b->setEnabled(on);
		sourceCombo->setEnabled(on);
	}

	void selectCurrent()
	{
		currentUuid = sourceCombo->currentData().toString().toStdString();
		obs_source_t *src = currentUuid.empty() ? nullptr : obs_get_source_by_uuid(currentUuid.c_str());
		view->setSource(src);
		obs_source_release(src);
		syncMode();
	}

	void syncMode()
	{
		obs_source_t *src = view->source();
		if (!src)
			return;
		calldata_t cd = {};
		calldata_init(&cd);
		if (proc_handler_call(obs_source_get_proc_handler(src), "pm_get_mode", &cd)) {
			int mode = (int)calldata_int(&cd, "mode");
			if (auto *b = modeGroup->button(mode))
				if (!b->isChecked())
					b->setChecked(true);
		}
		calldata_free(&cd);
		obs_source_release(src);
	}

	void callSimple(const char *name)
	{
		obs_source_t *src = view->source();
		if (!src)
			return;
		calldata_t cd = {};
		calldata_init(&cd);
		proc_handler_call(obs_source_get_proc_handler(src), name, &cd);
		calldata_free(&cd);
		obs_source_release(src);
	}

	void addSurface(int shape)
	{
		obs_source_t *src = view->source();
		if (!src)
			return;
		calldata_t cd = {};
		calldata_init(&cd);
		calldata_set_int(&cd, "shape", shape);
		proc_handler_call(obs_source_get_proc_handler(src), "pm_add_surface", &cd);
		calldata_free(&cd);
		obs_source_release(src);
		/* Ny flate er lite nyttig i "Ferdig"-modus: gå til redigering. */
		if (modeGroup->checkedId() == 0)
			setMode(1);
	}

	void setMode(int mode)
	{
		obs_source_t *src = view->source();
		if (!src)
			return;
		calldata_t cd = {};
		calldata_init(&cd);
		calldata_set_int(&cd, "mode", mode);
		proc_handler_call(obs_source_get_proc_handler(src), "pm_set_mode", &cd);
		calldata_free(&cd);
		obs_source_release(src);
		if (auto *b = modeGroup->button(mode))
			b->setChecked(true);
	}
};

} // namespace

extern "C" void pm_dock_register(void)
{
	auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
	if (!main || !QCoreApplication::instance())
		return; /* Ingen OBS-hovedvindu (f.eks. i tester) */
	auto *dock = new MappingDock(main);
	if (!obs_frontend_add_dock_by_id("projection_mapping_dock", "Projection Mapping", dock))
		obs_log(LOG_WARNING, "Kunne ikke legge til Projection Mapping-panelet");
}
