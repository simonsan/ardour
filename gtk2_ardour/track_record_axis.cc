/*
 * Copyright (C) 2020 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <list>

#include <sigc++/bind.h>

#include "pbd/unwind.h"

#include "ardour/logmeter.h"
#include "ardour/meter.h"
#include "ardour/playlist.h"
#include "ardour/route.h"
#include "ardour/route_group.h"
#include "ardour/selection.h"
#include "ardour/session.h"
#include "ardour/track.h"

#include "ardour/audio_track.h"
#include "ardour/midi_track.h"

#include "gtkmm2ext/colors.h"
#include "gtkmm2ext/gtk_ui.h"
#include "gtkmm2ext/keyboard.h"
#include "gtkmm2ext/rgb_macros.h"
#include "gtkmm2ext/utils.h"

#include "widgets/tooltips.h"

#include "ardour_window.h"
#include "context_menu_helper.h"
#include "editor_cursors.h"
#include "group_tabs.h"
#include "gui_thread.h"
#include "level_meter.h"
#include "meter_patterns.h"
#include "public_editor.h"
#include "route_group_menu.h"
#include "timers.h"
#include "ui_config.h"
#include "utils.h"

#include "track_record_axis.h"

#include "pbd/i18n.h"

using namespace ARDOUR;
using namespace ArdourMeter;
using namespace ArdourWidgets;
using namespace ARDOUR_UI_UTILS;
using namespace PBD;
using namespace Gtk;
using namespace Gtkmm2ext;
using namespace std;

PBD::Signal1<void, TrackRecordAxis*> TrackRecordAxis::CatchDeletion;

#define PX_SCALE(pxmin, dflt) rint (std::max ((double)pxmin, (double)dflt* UIConfiguration::instance ().get_ui_scale ()))

bool TrackRecordAxis::_size_group_initialized = false;
Glib::RefPtr<Gtk::SizeGroup> TrackRecordAxis::_track_number_size_group;

TrackRecordAxis::TrackRecordAxis (Session* s, boost::shared_ptr<ARDOUR::Route> rt)
	: SessionHandlePtr (s)
	, RouteUI (s)
	, _clear_meters (true)
	, _route_ops_menu (0)
	, _input_button (true)
	, _playlist_button (S_("RTAV|P"))
	, _hseparator (1.0)
	, _vseparator (1.0)
	, _ctrls_button_size_group (Gtk::SizeGroup::create (Gtk::SIZE_GROUP_BOTH))
	, _monitor_ctrl_size_group (Gtk::SizeGroup::create (Gtk::SIZE_GROUP_BOTH))
	, _track_summary (rt)
{
	if (!_size_group_initialized) {
		_size_group_initialized = true;
		_track_number_size_group = Gtk::SizeGroup::create (Gtk::SIZE_GROUP_BOTH);
	}

	RouteUI::set_route (rt);

	_route->DropReferences.connect (_route_connections, invalidator (*this), boost::bind (&TrackRecordAxis::self_delete, this), gui_context ());

	UI::instance ()->theme_changed.connect (sigc::mem_fun (*this, &TrackRecordAxis::on_theme_changed));
	UIConfiguration::instance ().ColorsChanged.connect (sigc::mem_fun (*this, &TrackRecordAxis::on_theme_changed));
	UIConfiguration::instance ().DPIReset.connect (sigc::mem_fun (*this, &TrackRecordAxis::on_theme_changed));
	UIConfiguration::instance ().ParameterChanged.connect (sigc::mem_fun (*this, &TrackRecordAxis::parameter_changed));

	Config->ParameterChanged.connect (*this, invalidator (*this), ui_bind (&TrackRecordAxis::parameter_changed, this, _1), gui_context ());
	s->config.ParameterChanged.connect (*this, invalidator (*this), ui_bind (&TrackRecordAxis::parameter_changed, this, _1), gui_context ());

	PublicEditor::instance().playhead_cursor()->PositionChanged.connect (*this, invalidator (*this), boost::bind (&TrackSummary::playhead_position_changed, &_track_summary, _1), gui_context());

	ResetAllPeakDisplays.connect (sigc::mem_fun (*this, &TrackRecordAxis::reset_peak_display));
	ResetRoutePeakDisplays.connect (sigc::mem_fun (*this, &TrackRecordAxis::reset_route_peak_display));
	ResetGroupPeakDisplays.connect (sigc::mem_fun (*this, &TrackRecordAxis::reset_group_peak_display));

	_number_label.set_name ("tracknumber label");
	_number_label.set_elements ((ArdourButton::Element) (ArdourButton::Edge | ArdourButton::Body | ArdourButton::Text | ArdourButton::Inactive));
	_number_label.set_alignment (.5, .5);
	_number_label.set_fallthrough_to_parent (true);
	_number_label.signal_button_press_event().connect (sigc::mem_fun(*this, &TrackRecordAxis::route_ops_click), false);

	PropertyList* plist = new PropertyList();
	plist->add (ARDOUR::Properties::group_mute, true);
	plist->add (ARDOUR::Properties::group_solo, true);

	_playlist_button.set_name ("route button");
	_playlist_button.signal_button_press_event().connect (sigc::mem_fun(*this, &TrackRecordAxis::playlist_click), false);

	_level_meter = new LevelMeterVBox (s);
	_level_meter->set_meter (_route->shared_peak_meter ().get ());
	_level_meter->clear_meters ();
	_level_meter->setup_meters (120, 12);

	name_label.set_name (X_("TrackNameEditor"));
	name_label.set_alignment (0.0, 0.5);
	name_label.set_width_chars (12);

	_input_button.set_sizing_text ("Capture_8888");
	_input_button.set_route (rt, this);

	parameter_changed ("editor-stereo-only-meters");
	parameter_changed ("time-axis-name-ellipsize-mode");

	_ctrls.attach (_hseparator,           0, 10, 0, 1, Gtk::EXPAND|FILL, Gtk::SHRINK, 0, 0);
	_ctrls.attach (_number_label,         0,  1, 1, 2, Gtk::SHRINK,      Gtk::FILL,   4, 2);
	_ctrls.attach (_input_button,         1,  2, 1, 2, Gtk::SHRINK,      Gtk::SHRINK, 0, 2);
	_ctrls.attach (*rec_enable_button,    2,  3, 1, 2, Gtk::SHRINK,      Gtk::SHRINK, 2, 2);
	_ctrls.attach (_playlist_button,      3,  4, 1, 2, Gtk::SHRINK,      Gtk::SHRINK, 2, 2);
	_ctrls.attach (name_label,            4,  5, 1, 2, Gtk::FILL,        Gtk::SHRINK, 4, 2);
	_ctrls.attach (*monitor_input_button, 5,  6, 1, 2, Gtk::SHRINK,      Gtk::SHRINK, 1, 2);
	_ctrls.attach (*monitor_disk_button,  6,  7, 1, 2, Gtk::SHRINK,      Gtk::SHRINK, 1, 2);
	_ctrls.attach (*_level_meter,         7,  8, 1, 2, Gtk::SHRINK,      Gtk::SHRINK, 2, 2);
	_ctrls.attach (_vseparator,           8,  9, 1, 2, Gtk::SHRINK,      Gtk::FILL,   2, 0);
	_ctrls.attach (_track_summary,        9, 10, 1, 2, Gtk::EXPAND|FILL, Gtk::FILL,   4, 0);

	set_tooltip (*mute_button, _("Mute"));
	set_tooltip (*rec_enable_button, _("Record"));
	set_tooltip (_playlist_button, _("Playlist")); // playlist_tip ()

	set_name_label ();
	update_sensitivity ();

	_track_number_size_group->add_widget (_number_label);
	_ctrls_button_size_group->add_widget (*rec_enable_button);
	_ctrls_button_size_group->add_widget (*mute_button);
	_ctrls_button_size_group->add_widget (_playlist_button);
	_monitor_ctrl_size_group->add_widget (*monitor_input_button);
	_monitor_ctrl_size_group->add_widget (*monitor_disk_button);

	pack_start (_ctrls, false, false);

	rec_enable_button->show ();
	monitor_input_button->show ();
	monitor_disk_button->show ();
	mute_button->show ();
	_level_meter->show ();
	_playlist_button.show();
	_number_label.show ();
	name_label.show ();
	_input_button.show ();
	_track_summary.show ();
	_hseparator.show ();
	_vseparator.show ();
	_ctrls.show ();
}

TrackRecordAxis::~TrackRecordAxis ()
{
	delete _level_meter;
	delete _route_ops_menu;
	CatchDeletion (this);
}

void
TrackRecordAxis::self_delete ()
{
	delete this;
}

void
TrackRecordAxis::set_session (Session* s)
{
	RouteUI::set_session (s);
	if (!s) {
		return;
	}
	s->config.ParameterChanged.connect (*this, invalidator (*this), ui_bind (&TrackRecordAxis::parameter_changed, this, _1), gui_context ());
}

void
TrackRecordAxis::blink_rec_display (bool onoff)
{
	RouteUI::blink_rec_display (onoff);
}

std::string
TrackRecordAxis::state_id () const
{
	if (_route) {
		return string_compose ("recctrl %1", _route->id ().to_s ());
	} else {
		return string ();
	}
}

void
TrackRecordAxis::set_button_names ()
{
	mute_button->set_text (S_("Mute|M"));
#if 0
	monitor_input_button->set_text (S_("MonitorInput|I"));
	monitor_disk_button->set_text (S_("MonitorDisk|D"));
#else
	monitor_input_button->set_text (_("In"));
	monitor_disk_button->set_text (_("Disk"));
#endif

	/* Solo/Listen is N/A */
}

void
TrackRecordAxis::route_property_changed (const PropertyChange& what_changed)
{
	if (!what_changed.contains (ARDOUR::Properties::name)) {
		return;
	}
	ENSURE_GUI_THREAD (*this, &TrackRecordAxis::route_property_changed, what_changed);
	set_name_label ();
	set_tooltip (*_level_meter, _route->name ());
}

void
TrackRecordAxis::route_color_changed ()
{
	_number_label.set_fixed_colors (gdk_color_to_rgba (color ()), gdk_color_to_rgba (color ()));
}

void
TrackRecordAxis::on_theme_changed ()
{
}

void
TrackRecordAxis::on_size_request (Gtk::Requisition* r)
{
	VBox::on_size_request (r);
}

void
TrackRecordAxis::on_size_allocate (Gtk::Allocation& a)
{
	VBox::on_size_allocate (a);
}

void
TrackRecordAxis::parameter_changed (std::string const& p)
{
	if (p == "editor-stereo-only-meters") {
#if 0
		if (UIConfiguration::instance ().get_editor_stereo_only_meters ()) {
			_level_meter->set_max_audio_meter_count (2);
		} else {
			_level_meter->set_max_audio_meter_count (0);
		}
#endif
	} else if (p == "time-axis-name-ellipsize-mode") {
		set_name_ellipsize_mode ();
	}
}

string
TrackRecordAxis::name () const
{
	return _route->name ();
}

Gdk::Color
TrackRecordAxis::color () const
{
	return RouteUI::route_color ();
}

void
TrackRecordAxis::set_name_label ()
{
	string x = _route->name ();
	if (x != name_label.get_text ()) {
		name_label.set_text (x);
	}
	set_tooltip (name_label, _route->name ());

	const int64_t track_number = _route->track_number ();
	assert (track_number > 0);
	_number_label.set_text (PBD::to_string (track_number));
}

void
TrackRecordAxis::route_active_changed ()
{
	RouteUI::route_active_changed ();
	update_sensitivity ();
}

void
TrackRecordAxis::map_frozen ()
{
	RouteUI::map_frozen ();

	switch (track()->freeze_state()) {
		case Track::Frozen:
			_playlist_button.set_sensitive (false);
			break;
		default:
			_playlist_button.set_sensitive (true);
			break;
	}

	update_sensitivity ();
}

void
TrackRecordAxis::update_sensitivity ()
{
	bool en = _route->active ();
	monitor_input_button->set_sensitive (en);
	monitor_disk_button->set_sensitive (en);
	_input_button.set_sensitive (en);
	_ctrls.set_sensitive (en);

	if (!is_track() || track()->mode() != ARDOUR::Normal) {
		_playlist_button.set_sensitive (false);
	}
}

void
TrackRecordAxis::set_gui_extents (samplepos_t s, samplepos_t e)
{
	_track_summary.set_gui_extents (s, e);
}

bool
TrackRecordAxis::rec_extent (samplepos_t& s, samplepos_t& e) const
{
	return _track_summary.rec_extent (s, e);
}

int
TrackRecordAxis::summary_xpos () const
{
	return _ctrls.get_width () - _track_summary.get_width () - 4;
}

void
TrackRecordAxis::fast_update ()
{
	if (_clear_meters) {
		_level_meter->clear_meters ();
		_clear_meters = false;
	}
	_level_meter->update_meters ();
}

void
TrackRecordAxis::reset_route_peak_display (Route* route)
{
	if (_route && _route.get () == route) {
		reset_peak_display ();
	}
}

void
TrackRecordAxis::reset_group_peak_display (RouteGroup* group)
{
	if (_route && group == _route->route_group ()) {
		reset_peak_display ();
	}
}

void
TrackRecordAxis::reset_peak_display ()
{
	_route->shared_peak_meter ()->reset_max ();
	_clear_meters = true;
}

bool
TrackRecordAxis::playlist_click (GdkEventButton* ev)
{
	if (ev->button != 1) {
		return true;
	}

	build_playlist_menu ();
	_route->session ().selection().select_stripable_and_maybe_group (_route, false, true, 0);
	Gtkmm2ext::anchored_menu_popup (playlist_action_menu, &_playlist_button, "", 1, ev->time);

	return true;
}

bool
TrackRecordAxis::route_ops_click (GdkEventButton* ev)
{
	if (ev->button != 3 ) {
		return false;
	}

	build_route_ops_menu ();

	_route->session ().selection().select_stripable_and_maybe_group (_route, false, true, 0);

	Gtkmm2ext::anchored_menu_popup (_route_ops_menu, &_number_label, "", 1, ev->time);
	return true;
}

void
TrackRecordAxis::build_route_ops_menu ()
{
	using namespace Menu_Helpers;

	delete _route_ops_menu;
	_route_ops_menu = new Menu;
	_route_ops_menu->set_name ("ArdourContextMenu");

	MenuList& items = _route_ops_menu->items ();

	items.push_back (MenuElem (_("Color..."), sigc::mem_fun (*this, &RouteUI::choose_color)));
	items.push_back (MenuElem (_("Comments..."), sigc::mem_fun (*this, &RouteUI::open_comment_editor)));
	items.push_back (MenuElem (_("Inputs..."), sigc::mem_fun (*this, &RouteUI::edit_input_configuration)));
	items.push_back (MenuElem (_("Outputs..."), sigc::mem_fun (*this, &RouteUI::edit_output_configuration)));

	items.push_back (SeparatorElem());

	items.push_back (MenuElem (_("Rename..."), sigc::mem_fun(*this, &RouteUI::route_rename)));
	/* do not allow rename if the track is record-enabled */
	items.back().set_sensitive (!is_track() || !track()->rec_enable_control()->get_value());
}

/* ****************************************************************************/

TrackRecordAxis::TrackSummary::TrackSummary (boost::shared_ptr<ARDOUR::Route> r)
	: _start (0)
	, _end (480000)
	, _xscale (1)
	, _last_playhead (0)
	, _rec_updating (false)
	, _rec_active (false)
{
	_track = boost::dynamic_pointer_cast<Track> (r);
	assert (_track);

	_track->PlaylistChanged.connect (_connections, invalidator (*this), boost::bind (&TrackSummary::playlist_changed, this), gui_context ());
	_track->playlist()->ContentsChanged.connect (_connections, invalidator (*this), boost::bind (&TrackSummary::playlist_changed, this), gui_context ());
	_track->presentation_info().PropertyChanged.connect (_connections, invalidator (*this), boost::bind (&TrackSummary::property_changed, this, _1), gui_context ());

	_track->rec_enable_control()->Changed.connect (_connections, invalidator (*this), boost::bind (&TrackSummary::maybe_setup_rec_box, this), gui_context());
	_track->session().TransportStateChange.connect (_connections, invalidator (*this), boost::bind (&TrackSummary::maybe_setup_rec_box, this), gui_context());
	_track->session().TransportLooped.connect (_connections, invalidator (*this), boost::bind (&TrackSummary::maybe_setup_rec_box, this), gui_context());
	_track->session().RecordStateChanged.connect (_connections, invalidator (*this), boost::bind (&TrackSummary::maybe_setup_rec_box, this), gui_context());

}

TrackRecordAxis::TrackSummary::~TrackSummary ()
{
	_rec_active = false;
	if (_rec_updating) {
		_screen_update_connection.disconnect();
	}
}

void
TrackRecordAxis::TrackSummary::render (Cairo::RefPtr<Cairo::Context> const& cr, cairo_rectangle_t* r)
{
	cr->rectangle (r->x, r->y, r->width, r->height);
	cr->clip ();

	RouteGroup* g = _track->route_group ();
	if (g && g->is_color()) {
		Gtkmm2ext::set_source_rgba (cr, GroupTabs::group_color (g));
	} else {
		Gtkmm2ext::set_source_rgba (cr, _track->presentation_info ().color ());
	}

	double ht = r->height - 2;
	double yc = 1 + ht / 2.;
	cr->set_line_width (ht);

	_track->playlist()->foreach_region(sigc::bind (sigc::mem_fun (*this, &TrackSummary::render_region), cr, yc));

	/* Record Boxes */
	if (_rec_rects.size () > 0) {
		Gtkmm2ext::set_source_rgba (cr, UIConfiguration::instance().color_mod("recording rect", "recording_rect"));
		for (std::vector<RecInfo>::const_iterator i = _rec_rects.begin (); i != _rec_rects.end (); ++i) {
			const samplepos_t rs = i->capture_start;
			const samplecnt_t re = i->capture_end;
			if (re > rs) {
				cr->move_to (sample_to_xpos (rs), yc);
				cr->line_to (sample_to_xpos (re), yc);
				cr->stroke ();
			}
		}
	}

	/* Playhead */
	Gtkmm2ext::set_source_rgba (cr, UIConfiguration::instance().color ("play head"));
	const double phx = sample_to_xpos (PublicEditor::instance().playhead_cursor ()->current_sample());
	cr->set_line_width (1.0);
	cr->move_to (floor (phx) + .5, 0);
	cr->line_to (floor (phx) + .5, get_height());
	cr->stroke ();
	_last_playhead = phx;
}

void
TrackRecordAxis::TrackSummary::render_region (boost::shared_ptr<ARDOUR::Region> r, Cairo::RefPtr<Cairo::Context> const& cr, double y)
{
	const samplepos_t rp = r->position ();
	const samplecnt_t rl = r->length ();

	 if (rp > _start) {
		 cr->move_to (sample_to_xpos (rp), y);
	 } else {
		 cr->move_to (0, y);
	 }
	 if (rp + rl > _start) {
		 cr->line_to (sample_to_xpos (rp + rl), y);
		 cr->stroke ();
	 } else {
		 cr->begin_new_path ();
	 }
}

void
TrackRecordAxis::TrackSummary::maybe_setup_rec_box ()
{
	if (_track->session ().transport_stopped_or_stopping () || !(_track->session ().transport_rolling () || _track->session ().get_record_enabled ())) {
		/* stopped, or not roll/rec */
		if (_rec_updating) {
			_rec_rects.clear ();
			_screen_update_connection.disconnect();
			_rec_updating = false;
			_rec_active = false;
			set_dirty ();
		}
		return;
	}

	if (!_track->rec_enable_control()->get_value() || !_track->session ().actively_recording ()) {
		/* rolling but not (or no longer) recording [yet] */
		_rec_active = false;
		return;
	}

	if (!_rec_active) {
		const samplepos_t rs = _track->current_capture_start ();
		_rec_rects.push_back (RecInfo (rs, rs));
	}

	_rec_active = true;

	if (!_rec_updating) {
		_screen_update_connection.disconnect();
		_screen_update_connection = Timers::rapid_connect (sigc::mem_fun(*this, &TrackSummary::update_rec_box));
		_rec_updating = true;
	}
}

void
TrackRecordAxis::TrackSummary::update_rec_box ()
{
	if (_rec_active && _rec_rects.size () > 0) {
		RecInfo& rect = _rec_rects.back ();
		rect.capture_start = _track->current_capture_start ();
		rect.capture_end = _track->current_capture_end ();
		set_dirty ();
	}
}

void
TrackRecordAxis::TrackSummary::playhead_position_changed (samplepos_t p)
{
  int const o = _last_playhead;
  int const n = sample_to_xpos (p);
  if (o != n) {
    int a = max (2, min (o, n));
    int b = max (o, n);

		cairo_rectangle_t r;
		r.x = a - 2;
		r.y = 0;
		r.width = b - a + 4;
		r.height = get_height ();
		set_dirty (&r);
  }
}

void
TrackRecordAxis::TrackSummary::playlist_changed ()
{
	set_dirty ();
}

void
TrackRecordAxis::TrackSummary::property_changed (PropertyChange const& what_changed)
{
	if (what_changed.contains (Properties::color)) {
		set_dirty ();
	}
}

void
TrackRecordAxis::TrackSummary::on_size_request (Gtk::Requisition* req)
{
  req->width = 200;
  req->height = 16;
}

void
TrackRecordAxis::TrackSummary::on_size_allocate (Gtk::Allocation& a)
{
  CairoWidget::on_size_allocate (a);

	if (_end > _start) {
		_xscale = static_cast<double> (a.get_width ()) / (_end - _start);
	}
}

void
TrackRecordAxis::TrackSummary::set_gui_extents (samplepos_t start, samplepos_t end)
{
	if (_start == start && _end == end) {
		return;
	}
	_start = start;
	_end = end;
	_xscale = static_cast<double> (get_width ()) / (_end - _start);

	set_dirty ();
}

bool
TrackRecordAxis::TrackSummary::on_button_press_event (GdkEventButton* ev)
{
	if (_track->session ().actively_recording ()) {
		return false;
	}
	// use  _start + ev->x / _xscale
	_track->session ().request_locate (_start + (double) (_end - _start) * ev->x / get_width ());
	return true;
}

bool
TrackRecordAxis::TrackSummary::rec_extent (samplepos_t& start, samplepos_t& end) const
{
	if (_rec_rects.size () == 0) {
		return false;
	}
	for (std::vector<RecInfo>::const_iterator i = _rec_rects.begin (); i != _rec_rects.end (); ++i) {
		start = std::min (start, i->capture_start);
		end = std::max (end, i->capture_end);
	}
	return true;
}
