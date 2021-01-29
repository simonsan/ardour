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

#ifndef __gtk_ardour_recorder_ui_h__
#define __gtk_ardour_recorder_ui_h__

#include <boost/shared_ptr.hpp>
#include <list>
#include <vector>

#include <gtkmm/alignment.h>
#include <gtkmm/box.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/sizegroup.h>
#include <gtkmm/table.h>

#include "pbd/natsort.h"

#include "ardour/session_handle.h"
#include "ardour/circular_buffer.h"
#include "ardour/types.h"

#include "gtkmm2ext/bindings.h"
#include "gtkmm2ext/cairo_widget.h"

#include "widgets/ardour_button.h"
#include "widgets/ardour_spacer.h"
#include "widgets/frame.h"
#include "widgets/pane.h"
#include "widgets/tabbable.h"

#include "input_port_monitor.h"

class TrackRecordAxis;
class RecorderGroupTabs;

class RecorderUI : public ArdourWidgets::Tabbable, public ARDOUR::SessionHandlePtr, public PBD::ScopedConnectionList
{
public:
	RecorderUI ();
	~RecorderUI ();

	void set_session (ARDOUR::Session*);
	void cleanup ();

	XMLNode& get_state ();
	int set_state (const XMLNode&, int /* version */);

	Gtk::Window* use_own_window (bool and_fill_it);

	void spill_port (std::string const&);
	void add_track (std::string const&);

private:
	void load_bindings ();
	void register_actions ();
	void update_title ();
	void session_going_away ();
	void parameter_changed (std::string const&);
	void presentation_info_changed (PBD::PropertyChange const&);
	void gui_extents_changed ();

	void start_updating ();
	void stop_updating ();
	void update_meters ();
	void add_or_remove_io (ARDOUR::DataType, std::vector<std::string>, bool);
	void update_io_widget_labels ();

	void initial_track_display ();
	void add_routes (ARDOUR::RouteList&);
	void remove_route (TrackRecordAxis*);
	void update_rec_table_layout ();
	void update_spacer_width (Gtk::Allocation&, TrackRecordAxis*);

	void set_connections (std::string const&);
	void port_connected_or_disconnected (std::string, std::string);
	void port_pretty_name_changed (std::string);

	void meter_area_size_allocate (Gtk::Allocation&);
	void meter_area_size_request (GtkRequisition*);
	void meter_area_layout ();

	bool scroller_button_release (GdkEventButton*);

	void arm_all ();
	void arm_none ();
	void new_take ();
	void peak_reset ();

	void update_sensitivity ();
	void update_recordstate ();
	void new_track_for_port (ARDOUR::DataType, std::string const&);

	static int calc_columns (int child_width, int parent_width);

	Gtkmm2ext::Bindings*  bindings;
	Gtk::VBox            _content;
	Gtk::HBox            _toolbar;
	ArdourWidgets::VPane _pane;
	Gtk::ScrolledWindow  _rec_scroller;
	Gtk::VBox            _rec_container;
	Gtk::HBox            _rec_groups;
	Gtk::VBox            _rec_area;
	Gtk::ScrolledWindow  _meter_scroller;
	Gtk::VBox            _meter_area;
	Gtk::Table           _meter_table;
	Gtk::EventBox        _scroller_base;

	ArdourWidgets::ArdourHSpacer _toolbar_sep;
	ArdourWidgets::ArdourButton  _btn_rec_all;
	ArdourWidgets::ArdourButton  _btn_rec_none;
	ArdourWidgets::ArdourButton  _btn_new_take;
	ArdourWidgets::ArdourButton  _btn_peak_reset;

	int  _meter_box_width;
	int  _meter_area_cols;

	std::set<std::string> _spill_port_names;

	sigc::connection          _fast_screen_update_connection;
	PBD::ScopedConnectionList _engine_connections;

	class RecRuler : public CairoWidget , public ARDOUR::SessionHandlePtr
	{
		public:
			RecRuler ();

			void playhead_position_changed (ARDOUR::samplepos_t);
			void set_gui_extents (samplepos_t, samplepos_t);

		protected:
			void render (Cairo::RefPtr<Cairo::Context> const&, cairo_rectangle_t*);
			void on_size_request (Gtk::Requisition*);
			bool on_button_press_event (GdkEventButton*);

		private:
			Glib::RefPtr<Pango::Layout> _layout;
			int                         _time_width;
			int                         _time_height;
			ARDOUR::samplecnt_t         _left;
			ARDOUR::samplecnt_t         _right;
	};

	class InputPort : public Gtk::EventBox
	{
		public:
			InputPort (std::string const&, ARDOUR::DataType, RecorderUI*);
			~InputPort ();

			void set_frame_label (std::string const&);
			void set_connections (ARDOUR::WeakRouteList);
			void setup_name ();
			bool spill (bool);
			bool spilled () const;
			void update_rec_stat ();

			ARDOUR::DataType data_type () const;
			std::string const& name () const;

			void update (float, float); // FastMeter
			void update (float const*); // EventMeter
			void update (ARDOUR::CircularSampleBuffer&); // InputScope
			void update (ARDOUR::CircularEventBuffer&); // EventMonitor

			bool operator< (InputPort const& o) const {
				if (_dt == o._dt) {
					return PBD::naturally_less (_port_name.c_str (), o._port_name.c_str ());
				}
				return _dt < (uint32_t) o._dt;
			}

		private:
			void rename_port ();

			ARDOUR::DataType            _dt;
			InputPortMonitor            _monitor;
			Gtk::Alignment              _alignment;
			ArdourWidgets::Frame        _frame;
			Gtk::HBox                   _hbox;
			ArdourWidgets::ArdourButton _spill_button;
			ArdourWidgets::ArdourButton _name_button;
			Gtk::Label                  _name_label;
			ArdourWidgets::ArdourButton _add_button;
			std::string                 _port_name;
			ARDOUR::WeakRouteList       _connected_routes;

			static bool                         _size_groups_initialized;
			static Glib::RefPtr<Gtk::SizeGroup> _name_size_group;
			static Glib::RefPtr<Gtk::SizeGroup> _spill_size_group;
			static Glib::RefPtr<Gtk::SizeGroup> _button_size_group;
			static Glib::RefPtr<Gtk::SizeGroup> _monitor_size_group;
	};

	struct InputPortPtrSort {
		bool operator() (boost::shared_ptr<InputPort> const& a, boost::shared_ptr<InputPort> const& b) const {
			return *a < *b;
		}
	};

	RecRuler                     _ruler;
	Gtk::EventBox                _space;
	Gtk::HBox                    _ruler_box;
	ArdourWidgets::ArdourHSpacer _ruler_sep;

	RecorderGroupTabs*           _rec_group_tabs;

	typedef std::map<std::string, boost::shared_ptr<InputPort> > InputPortMap;

	InputPortMap                _input_ports;
	std::list<TrackRecordAxis*> _recorders;
	std::list<TrackRecordAxis*> _visible_recorders;

public:
	/* only for RecorderGroupTab */
	std::list<TrackRecordAxis*> visible_recorders () const;
};

#endif /* __gtk_ardour_recorder_ui_h__ */
