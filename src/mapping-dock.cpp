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
 * Brukergrensesnittet:
 *  - Panelet "Projection Mapping" (dock) med egen visning og knapper.
 *  - Redigering direkte i OBS sin forhåndsvisning (museklikk fanges opp
 *    når de treffer en flate, ellers oppfører OBS seg som vanlig).
 *  - Tab bytter modus (Move -> Mask -> Ferdig), Cmd/Ctrl+Z angrer.
 *  - En liten modusindikator nederst ved forhåndsvisningen.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-interaction.h>
#include <graphics/matrix4.h>
#include <graphics/vec3.h>
#include <plugin-support.h>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QAbstractSpinBox>
#include <QApplication>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#define PM_ID "projection_mapping_source"

extern "C" void pm_set_editor_render(bool enabled);

namespace {

enum { MODE_SHOW = 0, MODE_MOVE = 1, MODE_CROP = 2, MODE_MASK = 3 };

/* ------------------------------------------------------------------ */
/* Hjelpefunksjoner                                                     */

bool isMapping(obs_source_t *src)
{
	return src && strcmp(obs_source_get_unversioned_id(src), PM_ID) == 0;
}

/* Alle mapping-kilder (med ekstra referanse – husk obs_source_release). */
std::vector<obs_source_t *> allMappings()
{
	std::vector<obs_source_t *> list;
	obs_enum_sources(
		[](void *param, obs_source_t *src) {
			if (isMapping(src))
				static_cast<std::vector<obs_source_t *> *>(param)->push_back(obs_source_get_ref(src));
			return true;
		},
		&list);
	return list;
}

void releaseAll(std::vector<obs_source_t *> &list)
{
	for (auto *s : list)
		obs_source_release(s);
	list.clear();
}

int getMode(obs_source_t *src)
{
	calldata_t cd = {};
	calldata_init(&cd);
	int mode = MODE_SHOW;
	if (proc_handler_call(obs_source_get_proc_handler(src), "pm_get_mode", &cd))
		mode = (int)calldata_int(&cd, "mode");
	calldata_free(&cd);
	return mode;
}

void setMode(obs_source_t *src, int mode)
{
	calldata_t cd = {};
	calldata_init(&cd);
	calldata_set_int(&cd, "mode", mode);
	proc_handler_call(obs_source_get_proc_handler(src), "pm_set_mode", &cd);
	calldata_free(&cd);
}

void callSimple(obs_source_t *src, const char *name)
{
	if (!src)
		return;
	calldata_t cd = {};
	calldata_init(&cd);
	proc_handler_call(obs_source_get_proc_handler(src), name, &cd);
	calldata_free(&cd);
}

int nextMode(int mode)
{
	switch (mode) {
	case MODE_MOVE:
		return MODE_MASK;
	case MODE_MASK:
		return MODE_SHOW;
	default:
		return MODE_MOVE;
	}
}

uint32_t toInteract(Qt::KeyboardModifiers m)
{
	uint32_t r = 0;
	if (m & Qt::ShiftModifier)
		r |= INTERACT_SHIFT_KEY;
	if (m & Qt::AltModifier)
		r |= INTERACT_ALT_KEY;
#ifdef __APPLE__
	if (m & Qt::ControlModifier)
		r |= INTERACT_COMMAND_KEY;
	if (m & Qt::MetaModifier)
		r |= INTERACT_CONTROL_KEY;
#else
	if (m & Qt::ControlModifier)
		r |= INTERACT_CONTROL_KEY;
#endif
	return r;
}

void sendClick(obs_source_t *src, int x, int y, uint32_t mods, bool up, int clicks)
{
	obs_mouse_event ev = {};
	ev.modifiers = mods;
	ev.x = x;
	ev.y = y;
	obs_source_send_mouse_click(src, &ev, MOUSE_LEFT, up, (uint32_t)clicks);
}

void sendMove(obs_source_t *src, int x, int y, uint32_t mods)
{
	obs_mouse_event ev = {};
	ev.modifiers = mods;
	ev.x = x;
	ev.y = y;
	obs_source_send_mouse_move(src, &ev, false);
}

/* Den mapping-kilden som sist ble endret (for Cmd+Z). */
obs_source_t *lastEdited()
{
	auto list = allMappings();
	obs_source_t *best = nullptr;
	long long bestTime = -1;
	for (auto *s : list) {
		calldata_t cd = {};
		calldata_init(&cd);
		proc_handler_call(obs_source_get_proc_handler(s), "pm_get_last_edit", &cd);
		long long t = calldata_int(&cd, "time");
		calldata_free(&cd);
		if (t > bestTime) {
			bestTime = t;
			best = s;
		}
	}
	if (best)
		obs_source_get_ref(best);
	releaseAll(list);
	return best;
}

bool anyEditing()
{
	auto list = allMappings();
	bool editing = false;
	for (auto *s : list)
		if (getMode(s) != MODE_SHOW)
			editing = true;
	releaseAll(list);
	return editing;
}

void cycleAllModes()
{
	auto list = allMappings();
	if (!list.empty()) {
		int next = nextMode(getMode(list.front()));
		for (auto *s : list)
			setMode(s, next);
	}
	releaseAll(list);
}

void undoLast(bool redo)
{
	obs_source_t *s = lastEdited();
	if (!s)
		return;
	calldata_t cd = {};
	calldata_init(&cd);
	proc_handler_call(obs_source_get_proc_handler(s), redo ? "pm_redo" : "pm_undo", &cd);
	calldata_free(&cd);
	obs_source_release(s);
}

/* ------------------------------------------------------------------ */
/* Visning i panelet: egen OBS-display + museinteraksjon                */

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
		click(e, false, 1);
	}
	void mouseDoubleClickEvent(QMouseEvent *e) override { click(e, false, 2); }
	void mouseReleaseEvent(QMouseEvent *e) override { click(e, true, 1); }

	void mouseMoveEvent(QMouseEvent *e) override
	{
		obs_source_t *src = source();
		if (!src)
			return;
		int x, y;
		toSource(e->position(), x, y);
		sendMove(src, x, y, toInteract(e->modifiers()));
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

	/* Skrives av tegnetråden, leses av UI-tråden. */
	std::atomic<float> scale{1.0f};
	std::atomic<float> offsetX{0.0f};
	std::atomic<float> offsetY{0.0f};

	QSize pixelSize() const
	{
		qreal dpr = devicePixelRatioF();
		return QSize(std::max(1, (int)(width() * dpr)), std::max(1, (int)(height() * dpr)));
	}

	void toSource(const QPointF &pos, int &x, int &y) const
	{
		qreal dpr = devicePixelRatioF();
		float s = scale.load();
		if (s <= 0.0f)
			s = 1.0f;
		x = (int)(((float)(pos.x() * dpr) - offsetX.load()) / s);
		y = (int)(((float)(pos.y() * dpr) - offsetY.load()) / s);
	}

	void click(QMouseEvent *e, bool up, int clicks)
	{
		if (e->button() != Qt::LeftButton)
			return;
		obs_source_t *src = source();
		if (!src)
			return;
		int x, y;
		toSource(e->position(), x, y);
		sendClick(src, x, y, toInteract(e->modifiers()), up, clicks);
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
			pm_set_editor_render(true);
			obs_source_video_render(src);
			pm_set_editor_render(false);
			gs_projection_pop();
			gs_viewport_pop();
		}
		obs_source_release(src);
	}
};

/* ------------------------------------------------------------------ */
/* Panelet                                                              */

const char *modeHint(int mode)
{
	switch (mode) {
	case MODE_MOVE:
		return "Dra hjørnene for perspektiv, kantene for å flytte en side, midt i flaten for å flytte hele. "
		       "Tab = neste modus.";
	case MODE_MASK:
		return "Dobbeltklikk på den røde linjen = nytt punkt. Dobbeltklikk på et punkt = fjern det. "
		       "Hold Alt/⌥ og tegn = legg til. Alt/⌥ + Shift og tegn = visk ut. Cmd/Ctrl+Z angrer.";
	case MODE_CROP:
		return "Dra rammene for å velge hvilken del av innholdet hver flate viser.";
	default:
		return "Hjelpelinjene er skjult. Trykk Tab eller Move for å redigere.";
	}
}

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

		/* Modus */
		auto *modeRow = new QHBoxLayout();
		modeGroup = new QButtonGroup(this);
		modeGroup->setExclusive(true);
		struct {
			const char *text;
			int mode;
			const char *tip;
		} modes[] = {
			{"● Move", MODE_MOVE, "Dra hjørner, kanter og flater (gul)"},
			{"● Mask", MODE_MASK, "Tegn og rediger masker (rød)"},
			{"Utsnitt", MODE_CROP, "Velg hvilken del av innholdet hver flate viser"},
			{"Ferdig", MODE_SHOW, "Skjul alle hjelpelinjer"},
		};
		for (auto &m : modes) {
			auto *b = new QPushButton(m.text, this);
			b->setCheckable(true);
			b->setToolTip(m.tip);
			if (m.mode == MODE_MOVE)
				b->setStyleSheet("QPushButton:checked { color: #ffd000; font-weight: bold; }");
			else if (m.mode == MODE_MASK)
				b->setStyleSheet("QPushButton:checked { color: #ff4040; font-weight: bold; }");
			else
				b->setStyleSheet("QPushButton:checked { font-weight: bold; }");
			modeGroup->addButton(b, m.mode);
			modeRow->addWidget(b);
		}
		root->addLayout(modeRow);

		/* Flater */
		auto *row1 = new QHBoxLayout();
		auto *addQuad = new QPushButton("+ Firkant", this);
		auto *addRound = new QPushButton("+ Rund", this);
		auto *toggle = new QPushButton("Rund ⇄ firkant", this);
		auto *remove = new QPushButton("Fjern", this);
		addQuad->setToolTip("Legg til en ny firkantet flate");
		addRound->setToolTip("Legg til en ny rund flate");
		toggle->setToolTip("Gjør valgt flate rund eller firkantet");
		remove->setToolTip("Fjern valgt flate (eller trykk Delete i panelet)");
		row1->addWidget(addQuad);
		row1->addWidget(addRound);
		row1->addWidget(toggle);
		row1->addWidget(remove);
		root->addLayout(row1);

		/* Avrunding */
		auto *row2 = new QHBoxLayout();
		auto *radiusLabel = new QLabel("Avrunding", this);
		radiusSlider = new QSlider(Qt::Horizontal, this);
		radiusSlider->setRange(0, 100);
		radiusSlider->setToolTip("Avrund hjørnene på valgt flate (0 % = skarpe, 100 % = helt runde)");
		radiusValue = new QLabel("0 %", this);
		radiusValue->setMinimumWidth(40);
		row2->addWidget(radiusLabel);
		row2->addWidget(radiusSlider, 1);
		row2->addWidget(radiusValue);
		root->addLayout(row2);

		/* Maske, nullstill, angre */
		auto *row3 = new QHBoxLayout();
		auto *clearMask = new QPushButton("Fjern maske", this);
		auto *reset = new QPushButton("Nullstill", this);
		undoBtn = new QPushButton("↶ Angre", this);
		redoBtn = new QPushButton("↷ Gjør om", this);
		clearMask->setToolTip("Fjern all maskering på valgt flate");
		reset->setToolTip("Sett valgt flate tilbake til start");
		undoBtn->setToolTip("Angre (Cmd/Ctrl+Z)");
		redoBtn->setToolTip("Gjør om (Cmd/Ctrl+Shift+Z)");
		row3->addWidget(clearMask);
		row3->addWidget(reset);
		row3->addWidget(undoBtn);
		row3->addWidget(redoBtn);
		root->addLayout(row3);

		guidesCheck = new QCheckBox("Vis hjelpelinjer på projektor", this);
		guidesCheck->setToolTip("Av: linjene vises bare i OBS og i panelet, aldri på projektoren");
		root->addWidget(guidesCheck);

		view = new MappingView(this);
		root->addWidget(view, 1);

		hint = new QLabel(this);
		hint->setWordWrap(true);
		hint->setStyleSheet("color: #9a9a9a;");
		root->addWidget(hint);

		connect(addQuad, &QPushButton::clicked, this, [this]() { addSurface(0); });
		connect(addRound, &QPushButton::clicked, this, [this]() { addSurface(1); });
		connect(toggle, &QPushButton::clicked, this, [this]() { simple("pm_toggle_shape"); });
		connect(remove, &QPushButton::clicked, this, [this]() { simple("pm_remove_surface"); });
		connect(reset, &QPushButton::clicked, this, [this]() { simple("pm_reset_surface"); });
		connect(clearMask, &QPushButton::clicked, this, [this]() { simple("pm_clear_mask"); });
		connect(undoBtn, &QPushButton::clicked, this, [this]() { simple("pm_undo"); });
		connect(redoBtn, &QPushButton::clicked, this, [this]() { simple("pm_redo"); });
		connect(modeGroup, &QButtonGroup::idClicked, this, [this](int mode) { applyMode(mode); });
		connect(sourceCombo, &QComboBox::currentIndexChanged, this, [this](int) { selectCurrent(); });
		connect(radiusSlider, &QSlider::sliderPressed, this, [this]() { simple("pm_push_undo"); });
		connect(radiusSlider, &QSlider::valueChanged, this, [this](int v) { setRadius(v); });
		connect(guidesCheck, &QCheckBox::toggled, this, [this](bool on) { setGuides(on); });
		view->onRemoveKey = [this]() {
			simple("pm_remove_surface");
		};

		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this]() { refresh(); });
		timer->start(300);
		refresh();
	}

private:
	QComboBox *sourceCombo;
	QButtonGroup *modeGroup;
	QSlider *radiusSlider;
	QLabel *radiusValue;
	QPushButton *undoBtn;
	QPushButton *redoBtn;
	QCheckBox *guidesCheck;
	MappingView *view;
	QLabel *hint;
	QTimer *timer;
	std::string currentUuid;

	/* Oppdaterer kildelisten og alle kontroller fra kilden. */
	void refresh()
	{
		auto list = allMappings();

		bool changed = (int)list.size() != sourceCombo->count();
		for (int i = 0; !changed && i < (int)list.size(); i++) {
			if (sourceCombo->itemData(i).toString().toStdString() != obs_source_get_uuid(list[i]) ||
			    sourceCombo->itemText(i).toStdString() != obs_source_get_name(list[i]))
				changed = true;
		}

		if (changed) {
			QSignalBlocker block(sourceCombo);
			sourceCombo->clear();
			int keep = -1;
			for (int i = 0; i < (int)list.size(); i++) {
				sourceCombo->addItem(QString::fromUtf8(obs_source_get_name(list[i])),
						     QString::fromUtf8(obs_source_get_uuid(list[i])));
				if (currentUuid == obs_source_get_uuid(list[i]))
					keep = i;
			}
			if (keep < 0 && !list.empty())
				keep = 0;
			sourceCombo->setCurrentIndex(keep);
			selectCurrent();
		}

		bool have = !list.empty();
		releaseAll(list);

		for (auto *w : findChildren<QWidget *>())
			if (w != hint && w != view)
				w->setEnabled(have);
		if (!have) {
			hint->setText("Legg til kilden «Projection Mapping» i en scene (trykk + under Kilder), "
				      "så dukker den opp her.");
			return;
		}
		sync();
	}

	void sync()
	{
		obs_source_t *src = view->source();
		if (!src)
			return;

		int mode = getMode(src);
		if (auto *b = modeGroup->button(mode))
			if (!b->isChecked())
				b->setChecked(true);
		hint->setText(QString::fromUtf8(modeHint(mode)));

		calldata_t cd = {};
		calldata_init(&cd);
		proc_handler_call(obs_source_get_proc_handler(src), "pm_get_selected", &cd);
		int radius = (int)calldata_int(&cd, "radius");
		bool round = calldata_int(&cd, "shape") == 1;
		bool hasSurface = calldata_int(&cd, "index") >= 0;
		calldata_free(&cd);

		radiusSlider->setEnabled(hasSurface && !round);
		if (!radiusSlider->isSliderDown()) {
			QSignalBlocker block(radiusSlider);
			radiusSlider->setValue(radius);
		}
		radiusValue->setText(round ? QString("rund") : QString("%1 %").arg(radiusSlider->value()));

		calldata_init(&cd);
		proc_handler_call(obs_source_get_proc_handler(src), "pm_get_last_edit", &cd);
		undoBtn->setEnabled(calldata_int(&cd, "undo_count") > 0);
		redoBtn->setEnabled(calldata_int(&cd, "redo_count") > 0);
		calldata_free(&cd);

		obs_data_t *settings = obs_source_get_settings(src);
		{
			QSignalBlocker block(guidesCheck);
			guidesCheck->setChecked(obs_data_get_bool(settings, "guides_on_output"));
		}
		obs_data_release(settings);
		obs_source_release(src);
	}

	void selectCurrent()
	{
		currentUuid = sourceCombo->currentData().toString().toStdString();
		obs_source_t *src = currentUuid.empty() ? nullptr : obs_get_source_by_uuid(currentUuid.c_str());
		view->setSource(src);
		obs_source_release(src);
		sync();
	}

	void simple(const char *name)
	{
		obs_source_t *src = view->source();
		callSimple(src, name);
		obs_source_release(src);
		sync();
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
		/* En ny flate er lite nyttig i "Ferdig": gå til Move. */
		if (getMode(src) == MODE_SHOW)
			setMode(src, MODE_MOVE);
		obs_source_release(src);
		sync();
	}

	void applyMode(int mode)
	{
		obs_source_t *src = view->source();
		if (!src)
			return;
		setMode(src, mode);
		obs_source_release(src);
		sync();
	}

	void setRadius(int v)
	{
		obs_source_t *src = view->source();
		if (!src)
			return;
		calldata_t cd = {};
		calldata_init(&cd);
		calldata_set_int(&cd, "radius", v);
		/* Under dra er angre-punktet allerede lagret ved sliderPressed. */
		calldata_set_bool(&cd, "push_undo", !radiusSlider->isSliderDown());
		proc_handler_call(obs_source_get_proc_handler(src), "pm_set_radius", &cd);
		calldata_free(&cd);
		obs_source_release(src);
		radiusValue->setText(QString("%1 %").arg(v));
	}

	void setGuides(bool on)
	{
		obs_source_t *src = view->source();
		if (!src)
			return;
		calldata_t cd = {};
		calldata_init(&cd);
		calldata_set_bool(&cd, "enabled", on);
		proc_handler_call(obs_source_get_proc_handler(src), "pm_set_guides", &cd);
		calldata_free(&cd);
		obs_source_release(src);
	}
};

/* ------------------------------------------------------------------ */
/* Redigering i OBS sin forhåndsvisning + hurtigtaster                  */

struct MappingItem {
	obs_source_t *source; /* med referanse */
	matrix4 toCanvas;     /* kildepiksel -> lerret */
};

void collectItems(obs_scene_t *scene, const matrix4 &parent, int depth, std::vector<MappingItem> &out)
{
	if (!scene || depth > 8)
		return;
	struct Ctx {
		const matrix4 *parent;
		int depth;
		std::vector<MappingItem> *out;
	} ctx = {&parent, depth, &out};

	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<Ctx *>(param);
			if (!obs_sceneitem_visible(item))
				return true;
			matrix4 local, world;
			obs_sceneitem_get_draw_transform(item, &local);
			matrix4_mul(&world, &local, c->parent);

			obs_source_t *src = obs_sceneitem_get_source(item);
			if (isMapping(src)) {
				c->out->push_back({obs_source_get_ref(src), world});
			} else if (obs_sceneitem_is_group(item)) {
				collectItems(obs_sceneitem_group_get_scene(item), world, c->depth + 1, *c->out);
			} else if (obs_scene_t *nested = obs_scene_from_source(src)) {
				collectItems(nested, world, c->depth + 1, *c->out);
			}
			return true;
		},
		&ctx);
}

class PreviewController : public QObject {
public:
	PreviewController(QMainWindow *main) : QObject(main), main(main)
	{
		attach();
		auto *t = new QTimer(this);
		connect(t, &QTimer::timeout, this, [this]() {
			attach();
			updateIndicator();
		});
		t->start(300);
		qApp->installEventFilter(this);
	}

	~PreviewController() override
	{
		if (qApp)
			qApp->removeEventFilter(this);
		clearActive();
	}

	void addEditorWindow(QWidget *w) { editors.push_back(w); }

protected:
	bool eventFilter(QObject *obj, QEvent *e) override
	{
		switch (e->type()) {
		case QEvent::ShortcutOverride:
		case QEvent::KeyPress:
			return handleKey(static_cast<QKeyEvent *>(e));
		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick:
		case QEvent::MouseMove:
		case QEvent::MouseButtonRelease:
			if (preview && obj == preview)
				return handleMouse(static_cast<QMouseEvent *>(e));
			return false;
		default:
			return false;
		}
	}

private:
	QMainWindow *main;
	QPointer<QWidget> preview;
	QPointer<QComboBox> scalingMode;
	QPointer<QLabel> indicator;
	std::vector<QPointer<QWidget>> editors;

	obs_weak_source_t *active = nullptr;
	matrix4 activeInv;

	void attach()
	{
		if (!preview)
			preview = main->findChild<QWidget *>("preview");
		if (!scalingMode)
			scalingMode = main->findChild<QComboBox *>("previewScalingMode");
		if (!indicator) {
			if (auto *bar = main->findChild<QWidget *>("previewXContainer")) {
				if (auto *layout = qobject_cast<QBoxLayout *>(bar->layout())) {
					indicator = new QLabel(bar);
					indicator->setTextFormat(Qt::RichText);
					indicator->setContentsMargins(6, 0, 10, 0);
					indicator->hide();
					layout->insertWidget(0, indicator);
				}
			}
		}
	}

	void updateIndicator()
	{
		if (!indicator)
			return;
		auto list = allMappings();
		int mode = MODE_SHOW;
		for (auto *s : list) {
			int m = getMode(s);
			if (m != MODE_SHOW) {
				mode = m;
				break;
			}
		}
		releaseAll(list);

		const char *color = nullptr, *name = nullptr;
		switch (mode) {
		case MODE_MOVE:
			color = "#ffd000";
			name = "Move";
			break;
		case MODE_MASK:
			color = "#ff3030";
			name = "Mask";
			break;
		case MODE_CROP:
			color = "#00d0ff";
			name = "Utsnitt";
			break;
		default:
			break;
		}
		if (!name) {
			indicator->hide();
			return;
		}
		QString tip = "Projection Mapping: Tab bytter modus.";
		if (!fitMode())
			tip += " Velg «Tilpass til vindu» (skalering) for å dra direkte i forhåndsvisningen.";
		indicator->setText(
			QString("<span style='color:%1; font-size:14px;'>●</span> <b>%2</b>").arg(color, name));
		indicator->setToolTip(tip);
		indicator->show();
	}

	bool fitMode() const { return !scalingMode || scalingMode->currentIndex() == 0; }

	/* Widgetposisjon -> lerretskoordinater (samme regnestykke som OBS bruker). */
	bool toCanvas(const QPointF &pos, float &cx, float &cy) const
	{
		if (!preview || !fitMode())
			return false;
		obs_video_info ovi;
		if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height)
			return false;

		const int edge = 10; /* PREVIEW_EDGE_SIZE i OBS */
		qreal dpr = preview->devicePixelRatioF();
		QSize px = preview->size() * dpr;
		int wcx = px.width() - edge * 2, wcy = px.height() - edge * 2;
		if (wcx <= 0 || wcy <= 0)
			return false;

		double windowAspect = double(wcx) / double(wcy);
		double baseAspect = double(ovi.base_width) / double(ovi.base_height);
		float scale;
		int newCX, newCY;
		if (windowAspect > baseAspect) {
			scale = float(wcy) / float(ovi.base_height);
			newCX = int(double(wcy) * baseAspect);
			newCY = wcy;
		} else {
			scale = float(wcx) / float(ovi.base_width);
			newCX = wcx;
			newCY = int(float(wcx) / baseAspect);
		}
		float x0 = float(wcx / 2 - newCX / 2 + edge);
		float y0 = float(wcy / 2 - newCY / 2 + edge);

		cx = (float(pos.x() * dpr) - x0) / scale;
		cy = (float(pos.y() * dpr) - y0) / scale;
		return true;
	}

	static void toLocal(const matrix4 &inv, float cx, float cy, int &x, int &y)
	{
		vec3 in, out;
		vec3_set(&in, cx, cy, 0.0f);
		vec3_transform(&out, &in, &inv);
		x = (int)lroundf(out.x);
		y = (int)lroundf(out.y);
	}

	void clearActive()
	{
		obs_weak_source_release(active);
		active = nullptr;
	}

	bool handleMouse(QMouseEvent *e)
	{
		uint32_t mods = toInteract(e->modifiers());

		if (e->type() == QEvent::MouseMove || e->type() == QEvent::MouseButtonRelease) {
			if (!active)
				return false;
			obs_source_t *src = obs_weak_source_get_source(active);
			float cx, cy;
			if (src && toCanvas(e->position(), cx, cy)) {
				int x, y;
				toLocal(activeInv, cx, cy, x, y);
				if (e->type() == QEvent::MouseMove)
					sendMove(src, x, y, mods);
				else if (e->button() == Qt::LeftButton)
					sendClick(src, x, y, mods, true, 1);
			}
			obs_source_release(src);
			if (e->type() == QEvent::MouseButtonRelease && e->button() == Qt::LeftButton)
				clearActive();
			return true;
		}

		/* Trykk / dobbeltklikk */
		if (e->button() != Qt::LeftButton)
			return false;
		float cx, cy;
		if (!toCanvas(e->position(), cx, cy))
			return false;

		obs_source_t *sceneSrc = obs_frontend_preview_program_mode_active()
						 ? obs_frontend_get_current_preview_scene()
						 : obs_frontend_get_current_scene();
		std::vector<MappingItem> items;
		matrix4 identity;
		matrix4_identity(&identity);
		collectItems(obs_scene_from_source(sceneSrc), identity, 0, items);
		obs_source_release(sceneSrc);

		bool consumed = false;
		/* Øverste element først. */
		for (auto it = items.rbegin(); it != items.rend() && !consumed; ++it) {
			if (getMode(it->source) == MODE_SHOW)
				continue;
			matrix4 inv;
			if (!matrix4_inv(&inv, &it->toCanvas))
				continue;
			int x, y;
			toLocal(inv, cx, cy, x, y);

			calldata_t cd = {};
			calldata_init(&cd);
			calldata_set_int(&cd, "x", x);
			calldata_set_int(&cd, "y", y);
			calldata_set_int(&cd, "mods", mods);
			proc_handler_call(obs_source_get_proc_handler(it->source), "pm_wants_click", &cd);
			bool hit = calldata_bool(&cd, "hit");
			calldata_free(&cd);

			if (hit) {
				clearActive();
				active = obs_source_get_weak_source(it->source);
				activeInv = inv;
				sendClick(it->source, x, y, mods, false,
					  e->type() == QEvent::MouseButtonDblClick ? 2 : 1);
				consumed = true;
			}
		}
		for (auto &it : items)
			obs_source_release(it.source);
		return consumed;
	}

	bool isTextInput(QWidget *w) const
	{
		if (!w)
			return false;
		if (qobject_cast<QLineEdit *>(w) || qobject_cast<QTextEdit *>(w) || qobject_cast<QPlainTextEdit *>(w) ||
		    qobject_cast<QAbstractSpinBox *>(w))
			return true;
		if (auto *cb = qobject_cast<QComboBox *>(w))
			return cb->isEditable();
		return false;
	}

	bool inOurWindows() const
	{
		QWidget *aw = QApplication::activeWindow();
		if (!aw)
			return false;
		if (aw == main)
			return true;
		for (auto &w : editors)
			if (w && w->window() == aw)
				return true;
		return false;
	}

	bool handleKey(QKeyEvent *e)
	{
		bool isTab = e->key() == Qt::Key_Tab && (e->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier;
		bool isUndo = e->matches(QKeySequence::Undo);
		bool isRedo = e->matches(QKeySequence::Redo);
		if (!isTab && !isUndo && !isRedo)
			return false;
		if (!inOurWindows() || isTextInput(QApplication::focusWidget()))
			return false;

		if (isTab) {
			if (e->type() != QEvent::KeyPress)
				return false;
			auto list = allMappings();
			bool have = !list.empty();
			releaseAll(list);
			if (!have)
				return false;
			cycleAllModes();
			updateIndicator();
			return true;
		}

		/* Angre/gjør om tas bare av mappingen mens den redigeres, ellers får OBS dem. */
		if (!anyEditing())
			return false;
		if (e->type() == QEvent::ShortcutOverride) {
			e->accept();
			return true;
		}
		if (!e->isAutoRepeat())
			undoLast(isRedo);
		return true;
	}
};

QPointer<PreviewController> controller;

} // namespace

extern "C" void pm_dock_register(void)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main || !QCoreApplication::instance())
		return; /* Ingen OBS-hovedvindu (f.eks. i tester) */

	auto *dock = new MappingDock(main);
	if (!obs_frontend_add_dock_by_id("projection_mapping_dock", "Projection Mapping", dock))
		obs_log(LOG_WARNING, "Kunne ikke legge til Projection Mapping-panelet");

	controller = new PreviewController(main);
	controller->addEditorWindow(dock);
}
