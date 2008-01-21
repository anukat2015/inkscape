/**
 * \brief Filter Effects dialog
 *
 * Authors:
 *   Nicholas Bishop <nicholasbishop@gmail.org>
 *
 * Copyright (C) 2007 Authors
 *
 * Released under GNU GPL.  Read the file 'COPYING' for more information.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <gtk/gtktreeview.h>
#include <gtkmm/cellrenderertext.h>
#include <gtkmm/colorbutton.h>
#include <gtkmm/messagedialog.h>
#include <gtkmm/paned.h>
#include <gtkmm/scale.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/spinbutton.h>
#include <gtkmm/stock.h>
#include <glibmm/i18n.h>

#include "application/application.h"
#include "application/editor.h"
#include "desktop.h"
#include "desktop-handles.h"
#include "dialog-manager.h"
#include "document.h"
#include "filter-chemistry.h"
#include "filter-effects-dialog.h"
#include "filter-enums.h"
#include "inkscape.h"
#include "path-prefix.h"
#include "selection.h"
#include "sp-feblend.h"
#include "sp-fecolormatrix.h"
#include "sp-fecomponenttransfer.h"
#include "sp-fecomposite.h"
#include "sp-feconvolvematrix.h"
#include "sp-fedisplacementmap.h"
#include "sp-fedistantlight.h"
#include "sp-femerge.h"
#include "sp-femergenode.h"
#include "sp-feoffset.h"
#include "sp-fepointlight.h"
#include "sp-fespotlight.h"
#include "sp-filter-primitive.h"
#include "sp-gaussian-blur.h"

#include "style.h"
#include "svg/svg-color.h"
#include "verbs.h"
#include "xml/node.h"
#include "xml/node-observer.h"
#include "xml/repr.h"
#include <sstream>

#include <iostream>

using namespace NR;

namespace Inkscape {
namespace UI {
namespace Dialog {

// Returns the number of inputs available for the filter primitive type
int input_count(const SPFilterPrimitive* prim)
{
    if(!prim)
        return 0;
    else if(SP_IS_FEBLEND(prim) || SP_IS_FECOMPOSITE(prim) || SP_IS_FEDISPLACEMENTMAP(prim))
        return 2;
    else if(SP_IS_FEMERGE(prim)) {
        // Return the number of feMergeNode connections plus an extra
        int count = 1;
        for(const SPObject* o = prim->firstChild(); o; o = o->next, ++count);
        return count;
    }
    else
        return 1;
}

// Very simple observer that just emits a signal if anything happens to a node
class FilterEffectsDialog::SignalObserver : public XML::NodeObserver
{
public:
    SignalObserver()
        : _oldsel(0)
    {}

    // Add this observer to the SPObject and remove it from any previous object
    void set(SPObject* o)
    {
        if(_oldsel && _oldsel->repr)
            _oldsel->repr->removeObserver(*this);
        if(o && o->repr)
            o->repr->addObserver(*this);
        _oldsel = o;
    }

    void notifyChildAdded(XML::Node&, XML::Node&, XML::Node*)
    { signal_changed()(); }

    void notifyChildRemoved(XML::Node&, XML::Node&, XML::Node*)
    { signal_changed()(); }

    void notifyChildOrderChanged(XML::Node&, XML::Node&, XML::Node*, XML::Node*)
    { signal_changed()(); }

    void notifyContentChanged(XML::Node&, Util::ptr_shared<char>, Util::ptr_shared<char>)
    {}

    void notifyAttributeChanged(XML::Node&, GQuark, Util::ptr_shared<char>, Util::ptr_shared<char>)
    { signal_changed()(); }

    sigc::signal<void>& signal_changed()
    {
        return _signal_changed;
    }
private:
    sigc::signal<void> _signal_changed;
    SPObject* _oldsel;
};

class CheckButtonAttr : public Gtk::CheckButton, public AttrWidget
{
public:
    CheckButtonAttr(const Glib::ustring& label,
                    const Glib::ustring& tv, const Glib::ustring& fv,
                    const SPAttributeEnum a)
        : Gtk::CheckButton(label),
          AttrWidget(a),
          _true_val(tv), _false_val(fv)
    {
        signal_toggled().connect(signal_attr_changed().make_slot());
    }

    Glib::ustring get_as_attribute() const
    {
        return get_active() ? _true_val : _false_val;
    }

    void set_from_attribute(SPObject* o)
    {
        const gchar* val = attribute_value(o);
        if(val) {
            if(_true_val == val)
                set_active(true);
            else if(_false_val == val)
                set_active(false);
        }
    }
private:
    const Glib::ustring _true_val, _false_val;
};

class SpinButtonAttr : public Gtk::SpinButton, public AttrWidget
{
public:
    SpinButtonAttr(double lower, double upper, double step_inc,
                   double climb_rate, int digits, const SPAttributeEnum a)
        : Gtk::SpinButton(climb_rate, digits),
          AttrWidget(a)
    {
        set_range(lower, upper);
        set_increments(step_inc, step_inc * 5);

        signal_value_changed().connect(signal_attr_changed().make_slot());
    }

    Glib::ustring get_as_attribute() const
    {
        const double val = get_value();
        
        if(get_digits() == 0)
            return Glib::Ascii::dtostr((int)val);
        else
            return Glib::Ascii::dtostr(val);
    }
    
    void set_from_attribute(SPObject* o)
    {
        const gchar* val = attribute_value(o);
        if(val)
            set_value(Glib::Ascii::strtod(val));
    }
};

// Contains an arbitrary number of spin buttons that use seperate attributes
class MultiSpinButton : public Gtk::HBox
{
public:
    MultiSpinButton(double lower, double upper, double step_inc,
                    double climb_rate, int digits, std::vector<SPAttributeEnum> attrs)
    {
        for(unsigned i = 0; i < attrs.size(); ++i) {
            _spins.push_back(new SpinButtonAttr(lower, upper, step_inc, climb_rate, digits, attrs[i]));
            pack_start(*_spins.back(), false, false);
        }
    }

    ~MultiSpinButton()
    {
        for(unsigned i = 0; i < _spins.size(); ++i)
            delete _spins[i];
    }

    std::vector<SpinButtonAttr*>& get_spinbuttons()
    {
        return _spins;
    }
private:
    std::vector<SpinButtonAttr*> _spins;
};

// Contains two spinbuttons that describe a NumberOptNumber
class DualSpinButton : public Gtk::HBox, public AttrWidget
{
public:
    DualSpinButton(double lower, double upper, double step_inc,
                   double climb_rate, int digits, const SPAttributeEnum a)
        : AttrWidget(a),
          _s1(climb_rate, digits), _s2(climb_rate, digits)
    {
        _s1.set_range(lower, upper);
        _s2.set_range(lower, upper);
        _s1.set_increments(step_inc, step_inc * 5);
        _s2.set_increments(step_inc, step_inc * 5);

        _s1.signal_value_changed().connect(signal_attr_changed().make_slot());
        _s2.signal_value_changed().connect(signal_attr_changed().make_slot());

        pack_start(_s1, false, false);
        pack_start(_s2, false, false);
    }

    Gtk::SpinButton& get_spinbutton1()
    {
        return _s1;
    }

    Gtk::SpinButton& get_spinbutton2()
    {
        return _s2;
    }

    virtual Glib::ustring get_as_attribute() const
    {
        double v1 = _s1.get_value();
        double v2 = _s2.get_value();
        
        if(_s1.get_digits() == 0) {
            v1 = (int)v1;
            v2 = (int)v2;
        }

        return Glib::Ascii::dtostr(v1) + " " + Glib::Ascii::dtostr(v2);
    }

    virtual void set_from_attribute(SPObject* o)
    {
        const gchar* val = attribute_value(o);
        if(val) {
            NumberOptNumber n;
            n.set(val);
            _s1.set_value(n.getNumber());
            _s2.set_value(n.getOptNumber());
        }
    }
private:
    Gtk::SpinButton _s1, _s2;
};

class ColorButton : public Gtk::ColorButton, public AttrWidget
{
public:
    ColorButton(const SPAttributeEnum a)
        : AttrWidget(a)
    {
        signal_color_set().connect(signal_attr_changed().make_slot());

        Gdk::Color col;
        col.set_rgb(65535, 65535, 65535);
        set_color(col);
    }
    
    // Returns the color in 'rgb(r,g,b)' form.
    Glib::ustring get_as_attribute() const
    {
        std::ostringstream os;
        const Gdk::Color c = get_color();
        const int r = c.get_red() / 257, g = c.get_green() / 257, b = c.get_blue() / 257;
        os << "rgb(" << r << "," << g << "," << b << ")";
        return os.str();
    }


    void set_from_attribute(SPObject* o)
    {
        const gchar* val = attribute_value(o);
        if(val) {
            const guint32 i = sp_svg_read_color(val, 0xFFFFFFFF);
            const int r = SP_RGBA32_R_U(i), g = SP_RGBA32_G_U(i), b = SP_RGBA32_B_U(i);
            Gdk::Color col;
            col.set_rgb(r * 257, g * 257, b * 257);
            set_color(col);
        }
    }
};

/* Displays/Edits the matrix for feConvolveMatrix or feColorMatrix */
class FilterEffectsDialog::MatrixAttr : public Gtk::Frame, public AttrWidget
{
public:
    MatrixAttr(const SPAttributeEnum a)
        : AttrWidget(a), _locked(false)
    {
        _model = Gtk::ListStore::create(_columns);
        _tree.set_model(_model);
        _tree.set_headers_visible(false);
        _tree.show();
        add(_tree);
        set_shadow_type(Gtk::SHADOW_IN);
    }

    std::vector<double> get_values() const
    {
        std::vector<double> vec;
        for(Gtk::TreeIter iter = _model->children().begin();
            iter != _model->children().end(); ++iter) {
            for(unsigned c = 0; c < _tree.get_columns().size(); ++c)
                vec.push_back((*iter)[_columns.cols[c]]);
        }
        return vec;
    }

    void set_values(const std::vector<double>& v)
    {
        unsigned i = 0;
        for(Gtk::TreeIter iter = _model->children().begin();
            iter != _model->children().end(); ++iter) {
            for(unsigned c = 0; c < _tree.get_columns().size(); ++c) {
                if(i >= v.size())
                    return;
                (*iter)[_columns.cols[c]] = v[i];
                ++i;
            }
        }
    }

    Glib::ustring get_as_attribute() const
    {
        std::ostringstream os;
        
        for(Gtk::TreeIter iter = _model->children().begin();
            iter != _model->children().end(); ++iter) {
            for(unsigned c = 0; c < _tree.get_columns().size(); ++c) {
                os << (*iter)[_columns.cols[c]] << " ";
            }
        }
        
        return os.str();
    }
    
    void set_from_attribute(SPObject* o)
    {
        if(o) {
            if(SP_IS_FECONVOLVEMATRIX(o)) {
                SPFeConvolveMatrix* conv = SP_FECONVOLVEMATRIX(o);
                int cols, rows;
                cols = (int)conv->order.getNumber();
                if(cols > 5)
                    cols = 5;
                rows = conv->order.optNumber_set ? (int)conv->order.getOptNumber() : cols;
                update(o, rows, cols);
            }
            else if(SP_IS_FECOLORMATRIX(o))
                update(o, 4, 5);
        }
    }
private:
    class MatrixColumns : public Gtk::TreeModel::ColumnRecord
    {
    public:
        MatrixColumns()
        {
            cols.resize(5);
            for(unsigned i = 0; i < cols.size(); ++i)
                add(cols[i]);
        }
        std::vector<Gtk::TreeModelColumn<double> > cols;
    };

    void update(SPObject* o, const int rows, const int cols)
    {
        if(_locked)
            return;

        _model->clear();

        _tree.remove_all_columns();

        std::vector<gdouble>* values = NULL;
        if(SP_IS_FECOLORMATRIX(o))
            values = &SP_FECOLORMATRIX(o)->values;
        else if(SP_IS_FECONVOLVEMATRIX(o))
            values = &SP_FECONVOLVEMATRIX(o)->kernelMatrix;
        else
            return;

        if(o) {
            int ndx = 0;

            for(int i = 0; i < cols; ++i) {
                _tree.append_column_numeric_editable("", _columns.cols[i], "%.2f");
                dynamic_cast<Gtk::CellRendererText*>(
                    _tree.get_column_cell_renderer(i))->signal_edited().connect(
                        sigc::mem_fun(*this, &MatrixAttr::rebind));
            }

            for(int r = 0; r < rows; ++r) {
                Gtk::TreeRow row = *(_model->append());
                // Default to identity matrix
                for(int c = 0; c < cols; ++c, ++ndx)
                    row[_columns.cols[c]] = ndx < (int)values->size() ? (*values)[ndx] : (r == c ? 1 : 0);
            }
        }
    }

    void rebind(const Glib::ustring&, const Glib::ustring&)
    {
        _locked = true;
        signal_attr_changed()();
        _locked = false;
    }

    bool _locked;
    Gtk::TreeView _tree;
    Glib::RefPtr<Gtk::ListStore> _model;
    MatrixColumns _columns;
};

// Displays a matrix or a slider for feColorMatrix
class FilterEffectsDialog::ColorMatrixValues : public Gtk::Frame, public AttrWidget
{
public:
    ColorMatrixValues()
        : AttrWidget(SP_ATTR_VALUES),
          _matrix(SP_ATTR_VALUES),
          _saturation(0, 0, 1, 0.1, 0.01, 2, SP_ATTR_VALUES),
          _angle(0, 0, 360, 0.1, 0.01, 1, SP_ATTR_VALUES),
          _label(_("None"), Gtk::ALIGN_LEFT),
          _use_stored(false),
          _saturation_store(0),
          _angle_store(0)
    {
        _matrix.signal_attr_changed().connect(signal_attr_changed().make_slot());
        _saturation.signal_attr_changed().connect(signal_attr_changed().make_slot());
        _angle.signal_attr_changed().connect(signal_attr_changed().make_slot());
        signal_attr_changed().connect(sigc::mem_fun(*this, &ColorMatrixValues::update_store));

        _matrix.show();
        _saturation.show();
        _angle.show();
        _label.show();
        _label.set_sensitive(false);

        set_shadow_type(Gtk::SHADOW_NONE);
    }

    virtual void set_from_attribute(SPObject* o)
    {
        if(SP_IS_FECOLORMATRIX(o)) {
            SPFeColorMatrix* col = SP_FECOLORMATRIX(o);
            remove();
            switch(col->type) {
                case COLORMATRIX_SATURATE:
                    add(_saturation);
                    if(_use_stored)
                        _saturation.set_value(_saturation_store);
                    else
                        _saturation.set_from_attribute(o);
                    break;
                case COLORMATRIX_HUEROTATE:
                    add(_angle);
                    if(_use_stored)
                        _angle.set_value(_angle_store);
                    else
                        _angle.set_from_attribute(o);
                    break;
                case COLORMATRIX_LUMINANCETOALPHA:
                    add(_label);
                    break;
                case COLORMATRIX_MATRIX:
                default:
                    add(_matrix);
                    if(_use_stored)
                        _matrix.set_values(_matrix_store);
                    else
                        _matrix.set_from_attribute(o);
                    break;
            }
            _use_stored = true;
        }
    }

    virtual Glib::ustring get_as_attribute() const
    {
        const Widget* w = get_child();
        if(w == &_label)
            return "";
        else
            return dynamic_cast<const AttrWidget*>(w)->get_as_attribute();
    }

    void clear_store()
    {
        _use_stored = false;
    }
private:
    void update_store()
    {
        const Widget* w = get_child();
        if(w == &_matrix)
            _matrix_store = _matrix.get_values();
        else if(w == &_saturation)
            _saturation_store = _saturation.get_value();
        else if(w == &_angle)
            _angle_store = _angle.get_value();
    }

    MatrixAttr _matrix;
    SpinSlider _saturation;
    SpinSlider _angle;
    Gtk::Label _label;

    // Store separate values for the different color modes
    bool _use_stored;
    std::vector<double> _matrix_store;
    double _saturation_store;
    double _angle_store;
};

class FilterEffectsDialog::Settings
{
public:
    typedef sigc::slot<void, const AttrWidget*> SetAttrSlot;

    Settings(FilterEffectsDialog& d, Gtk::Box& b, SetAttrSlot slot, const int maxtypes)
        : _dialog(d), _set_attr_slot(slot), _current_type(-1), _max_types(maxtypes)
    {
        _groups.resize(_max_types);
        _attrwidgets.resize(_max_types);

        for(int i = 0; i < _max_types; ++i) {
            _groups[i] = new Gtk::VBox;
            b.add(*_groups[i]);
        }
    }

    ~Settings()
    {
        for(int i = 0; i < _max_types; ++i) {
            delete _groups[i];
            for(unsigned j = 0; j < _attrwidgets[i].size(); ++j)
                delete _attrwidgets[i][j];
        }
    }

    // Show the active settings group and update all the AttrWidgets with new values
    void show_and_update(const int t, SPObject* ob)
    {
        if(t != _current_type) {
            type(t);
            for(unsigned i = 0; i < _groups.size(); ++i)
                _groups[i]->hide();
        }
        if(t >= 0)
            _groups[t]->show_all();

        _dialog.set_attrs_locked(true);
        for(unsigned i = 0; i < _attrwidgets[_current_type].size(); ++i)
            _attrwidgets[_current_type][i]->set_from_attribute(ob);
        _dialog.set_attrs_locked(false);
    }

    int get_current_type() const
    {
        return _current_type;
    }

    void type(const int t)
    {
        _current_type = t;
    }

    void add_notimplemented()
    {
        Gtk::Label* lbl = Gtk::manage(new Gtk::Label("This SVG filter effect is not yet implemented in Inkscape."));

        add_widget(lbl, "");
    }

    // LightSource
    LightSourceControl* add_lightsource();

    // CheckBox
    CheckButtonAttr* add_checkbutton(const SPAttributeEnum attr, const Glib::ustring& label,
                                     const Glib::ustring& tv, const Glib::ustring& fv)
    {
        CheckButtonAttr* cb = new CheckButtonAttr(label, tv, fv, attr);
        add_widget(cb, "");
        add_attr_widget(cb);
        return cb;
    }

    // ColorButton
    ColorButton* add_color(const SPAttributeEnum attr, const Glib::ustring& label)
    {
        ColorButton* col = new ColorButton(attr);
        add_widget(col, label);
        add_attr_widget(col);
        return col;
    }

    // Matrix
    MatrixAttr* add_matrix(const SPAttributeEnum attr, const Glib::ustring& label)
    {
        MatrixAttr* conv = new MatrixAttr(attr);
        add_widget(conv, label);
        add_attr_widget(conv);
        return conv;
    }

    // ColorMatrixValues
    ColorMatrixValues* add_colormatrixvalues(const Glib::ustring& label)
    {
        ColorMatrixValues* cmv = new ColorMatrixValues;
        add_widget(cmv, label);
        add_attr_widget(cmv);
        return cmv;
    }

    // SpinSlider
    SpinSlider* add_spinslider(const SPAttributeEnum attr, const Glib::ustring& label,
                         const double lo, const double hi, const double step_inc, const double climb, const int digits)
    {
        SpinSlider* spinslider = new SpinSlider(lo, lo, hi, step_inc, climb, digits, attr);
        add_widget(spinslider, label);
        add_attr_widget(spinslider);
        return spinslider;
    }

    // DualSpinSlider
    DualSpinSlider* add_dualspinslider(const SPAttributeEnum attr, const Glib::ustring& label,
                                       const double lo, const double hi, const double step_inc,
                                       const double climb, const int digits)
    {
        DualSpinSlider* dss = new DualSpinSlider(lo, lo, hi, step_inc, climb, digits, attr);
        add_widget(dss, label);
        add_attr_widget(dss);
        return dss;
    }

    // DualSpinButton
    DualSpinButton* add_dualspinbutton(const SPAttributeEnum attr, const Glib::ustring& label,
                                       const double lo, const double hi, const double step_inc,
                                       const double climb, const int digits)
    {
        DualSpinButton* dsb = new DualSpinButton(lo, hi, step_inc, climb, digits, attr);
        add_widget(dsb, label);
        add_attr_widget(dsb);
        return dsb;
    }

    // MultiSpinButton
    MultiSpinButton* add_multispinbutton(const SPAttributeEnum attr1, const SPAttributeEnum attr2,
                                         const Glib::ustring& label, const double lo, const double hi,
                                         const double step_inc, const double climb, const int digits)
    {
        std::vector<SPAttributeEnum> attrs;
        attrs.push_back(attr1);
        attrs.push_back(attr2);
        MultiSpinButton* msb = new MultiSpinButton(lo, hi, step_inc, climb, digits, attrs);
        add_widget(msb, label);
        for(unsigned i = 0; i < msb->get_spinbuttons().size(); ++i)
            add_attr_widget(msb->get_spinbuttons()[i]);
        return msb;
    }
    MultiSpinButton* add_multispinbutton(const SPAttributeEnum attr1, const SPAttributeEnum attr2,
                                         const SPAttributeEnum attr3, const Glib::ustring& label, const double lo,
                                         const double hi, const double step_inc, const double climb, const int digits)
    {
        std::vector<SPAttributeEnum> attrs;
        attrs.push_back(attr1);
        attrs.push_back(attr2);
        attrs.push_back(attr3);
        MultiSpinButton* msb = new MultiSpinButton(lo, hi, step_inc, climb, digits, attrs);
        add_widget(msb, label);
        for(unsigned i = 0; i < msb->get_spinbuttons().size(); ++i)
            add_attr_widget(msb->get_spinbuttons()[i]);
        return msb;
    }

    // ComboBoxEnum
    template<typename T> ComboBoxEnum<T>* add_combo(const SPAttributeEnum attr,
                                  const Glib::ustring& label,
                                  const Util::EnumDataConverter<T>& conv)
    {
        ComboBoxEnum<T>* combo = new ComboBoxEnum<T>(conv, attr);
        add_widget(combo, label);
        add_attr_widget(combo);
        return combo;
    }
private:
    void add_attr_widget(AttrWidget* a)
    {    
        _attrwidgets[_current_type].push_back(a);
        a->signal_attr_changed().connect(sigc::bind(_set_attr_slot, a));
    }

    /* Adds a new settings widget using the specified label. The label will be formatted with a colon
       and all widgets within the setting group are aligned automatically. */
    void add_widget(Gtk::Widget* w, const Glib::ustring& label)
    {
        Gtk::Label *lbl = 0;
        Gtk::HBox *hb = Gtk::manage(new Gtk::HBox);
        hb->set_spacing(12);
        
        if(label != "") {
            lbl = Gtk::manage(new Gtk::Label(label + (label == "" ? "" : ":"), Gtk::ALIGN_LEFT));
            hb->pack_start(*lbl, false, false);
            _dialog._sizegroup->add_widget(*lbl);
            lbl->show();
        }
        
        hb->pack_start(*w);
        _groups[_current_type]->pack_start(*hb);
        hb->show();
        w->show();
    }

    std::vector<Gtk::VBox*> _groups;

    FilterEffectsDialog& _dialog;
    SetAttrSlot _set_attr_slot;
    std::vector<std::vector<AttrWidget*> > _attrwidgets;
    int _current_type, _max_types;
};

// Settings for the three light source objects
class FilterEffectsDialog::LightSourceControl : public AttrWidget
{
public:
    LightSourceControl(FilterEffectsDialog& d)
        : AttrWidget(SP_ATTR_INVALID),
          _dialog(d),
          _settings(d, _box, sigc::mem_fun(_dialog, &FilterEffectsDialog::set_child_attr_direct), LIGHT_ENDSOURCE),
          _light_label(_("Light Source:"), Gtk::ALIGN_LEFT),
          _light_source(LightSourceConverter),
          _locked(false)
    {
        _light_box.pack_start(_light_label, false, false);
        _light_box.pack_start(_light_source);
        _light_box.show_all();
        _light_box.set_spacing(12);
        _dialog._sizegroup->add_widget(_light_label);

        _box.add(_light_box);
        _box.reorder_child(_light_box, 0);
        _light_source.signal_changed().connect(sigc::mem_fun(*this, &LightSourceControl::on_source_changed));

        // FIXME: these range values are complete crap

        _settings.type(LIGHT_DISTANT);
        _settings.add_spinslider(SP_ATTR_AZIMUTH, _("Azimuth"), 0, 360, 1, 1, 0);
        _settings.add_spinslider(SP_ATTR_ELEVATION, _("Elevation"), 0, 360, 1, 1, 0);

        _settings.type(LIGHT_POINT);
        _settings.add_multispinbutton(SP_ATTR_X, SP_ATTR_Y, SP_ATTR_Z, _("Location"), -99999, 99999, 1, 100, 0);

        _settings.type(LIGHT_SPOT);
        _settings.add_multispinbutton(SP_ATTR_X, SP_ATTR_Y, SP_ATTR_Z, _("Location"), -99999, 99999, 1, 100, 0);
        _settings.add_multispinbutton(SP_ATTR_POINTSATX, SP_ATTR_POINTSATY, SP_ATTR_POINTSATZ,
                                      _("Points At"), -99999, 99999, 1, 100, 0);
        _settings.add_spinslider(SP_ATTR_SPECULAREXPONENT, _("Specular Exponent"), 1, 100, 1, 1, 0);
        _settings.add_spinslider(SP_ATTR_LIMITINGCONEANGLE, _("Cone Angle"), 1, 100, 1, 1, 0);
    }

    Gtk::VBox& get_box()
    {
        return _box;
    }
protected:
    Glib::ustring get_as_attribute() const
    {
        return "";
    }
    void set_from_attribute(SPObject* o)
    {
        if(_locked)
            return;

        _locked = true;

        SPObject* child = o->children;
        
        if(SP_IS_FEDISTANTLIGHT(child))
            _light_source.set_active(0);
        else if(SP_IS_FEPOINTLIGHT(child))
            _light_source.set_active(1);
        else if(SP_IS_FESPOTLIGHT(child))
            _light_source.set_active(2);
        else
            _light_source.set_active(-1);

        update();

        _locked = false;
    }
private:
    void on_source_changed()
    {
        if(_locked)
            return;

        SPFilterPrimitive* prim = _dialog._primitive_list.get_selected();
        if(prim) {
            _locked = true;

            SPObject* child = prim->children;
            const int ls = _light_source.get_active_row_number();
            // Check if the light source type has changed
            if(!(ls == -1 && !child) &&
               !(ls == 0 && SP_IS_FEDISTANTLIGHT(child)) &&
               !(ls == 1 && SP_IS_FEPOINTLIGHT(child)) &&
               !(ls == 2 && SP_IS_FESPOTLIGHT(child))) {
                if(child)
                    sp_repr_unparent(child->repr);

                if(ls != -1) {
                    Inkscape::XML::Document *xml_doc = sp_document_repr_doc(prim->document);
                    Inkscape::XML::Node *repr = xml_doc->createElement(_light_source.get_active_data()->key.c_str());
                    prim->repr->appendChild(repr);
                }

                sp_document_done(prim->document, SP_VERB_DIALOG_FILTER_EFFECTS, _("New light source"));
                update();
            }

            _locked = false;
        }
    }

    void update()
    {
        _box.hide_all();
        _box.show();
        _light_box.show_all();
        
        SPFilterPrimitive* prim = _dialog._primitive_list.get_selected();
        if(prim && prim->children)
            _settings.show_and_update(_light_source.get_active_data()->id, prim->children);
    }

    FilterEffectsDialog& _dialog;
    Gtk::VBox _box;
    Settings _settings;
    Gtk::HBox _light_box;
    Gtk::Label _light_label;
    ComboBoxEnum<LightSource> _light_source;
    bool _locked;
};

FilterEffectsDialog::LightSourceControl* FilterEffectsDialog::Settings::add_lightsource()
{
    LightSourceControl* ls = new LightSourceControl(_dialog);
    add_attr_widget(ls);
    add_widget(&ls->get_box(), "");
    return ls;
}

Glib::RefPtr<Gtk::Menu> create_popup_menu(Gtk::Widget& parent, sigc::slot<void> dup,
                                          sigc::slot<void> rem)
{
    Glib::RefPtr<Gtk::Menu> menu(new Gtk::Menu);

    menu->items().push_back(Gtk::Menu_Helpers::MenuElem(_("_Duplicate"), dup));
    Gtk::MenuItem* mi = Gtk::manage(new Gtk::ImageMenuItem(Gtk::Stock::REMOVE));
    menu->append(*mi);
    mi->signal_activate().connect(rem);
    mi->show();
    menu->accelerate(parent);

    return menu;
}

/*** FilterModifier ***/
FilterEffectsDialog::FilterModifier::FilterModifier(FilterEffectsDialog& d)
    : _dialog(d), _add(Gtk::Stock::NEW), _observer(new SignalObserver)
{
    Gtk::ScrolledWindow* sw = Gtk::manage(new Gtk::ScrolledWindow);
    pack_start(*sw);
    pack_start(_add, false, false);
    sw->add(_list);

    _model = Gtk::ListStore::create(_columns);
    _list.set_model(_model);
    _cell_toggle.set_active(true);
    const int selcol = _list.append_column("", _cell_toggle);
    Gtk::TreeViewColumn* col = _list.get_column(selcol - 1);
    if(col)
       col->add_attribute(_cell_toggle.property_active(), _columns.sel);
    _list.append_column_editable(_("_Filter"), _columns.label);
    ((Gtk::CellRendererText*)_list.get_column(1)->get_first_cell_renderer())->
        signal_edited().connect(sigc::mem_fun(*this, &FilterEffectsDialog::FilterModifier::on_name_edited));

    sw->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    sw->set_shadow_type(Gtk::SHADOW_IN);
    show_all_children();
    _add.signal_clicked().connect(sigc::mem_fun(*this, &FilterModifier::add_filter));
    _cell_toggle.signal_toggled().connect(sigc::mem_fun(*this, &FilterModifier::on_selection_toggled));
    _list.signal_button_release_event().connect_notify(
        sigc::mem_fun(*this, &FilterModifier::filter_list_button_release));
    _menu = create_popup_menu(*this, sigc::mem_fun(*this, &FilterModifier::duplicate_filter),
                              sigc::mem_fun(*this, &FilterModifier::remove_filter));
    _menu->items().push_back(Gtk::Menu_Helpers::MenuElem(
                                 _("R_ename"), sigc::mem_fun(*this, &FilterModifier::rename_filter))); 	 
    _menu->accelerate(*this);

    _list.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &FilterModifier::on_filter_selection_changed));
    _observer->signal_changed().connect(signal_filter_changed().make_slot());
    g_signal_connect(G_OBJECT(INKSCAPE), "change_selection",
                     G_CALLBACK(&FilterModifier::on_inkscape_change_selection), this);

    g_signal_connect(G_OBJECT(INKSCAPE), "activate_desktop",
                     G_CALLBACK(&FilterModifier::on_activate_desktop), this);

    on_activate_desktop(INKSCAPE, d.getDesktop(), this);
    update_filters();
}

FilterEffectsDialog::FilterModifier::~FilterModifier()
{
   _resource_changed.disconnect();
   _doc_replaced.disconnect();
}

void FilterEffectsDialog::FilterModifier::on_activate_desktop(Application*, SPDesktop* desktop, FilterModifier* me)
{
    me->_doc_replaced.disconnect();
    me->_doc_replaced = desktop->connectDocumentReplaced(
        sigc::mem_fun(me, &FilterModifier::on_document_replaced));

    me->_resource_changed.disconnect();
    me->_resource_changed =
        sp_document_resources_changed_connect(sp_desktop_document(desktop), "filter",
                                              sigc::mem_fun(me, &FilterModifier::update_filters));

    me->_dialog.setDesktop(desktop);

    me->update_filters();
}


// When the selection changes, show the active filter(s) in the dialog
void FilterEffectsDialog::FilterModifier::on_inkscape_change_selection(Application */*inkscape*/,
                                                                       Selection *sel,
                                                                       FilterModifier* fm)
{
    if(fm && sel)
        fm->update_selection(sel);
}

// Update each filter's sel property based on the current object selection;
//  If the filter is not used by any selected object, sel = 0,
//  otherwise sel is set to the total number of filters in use by selected objects
//  If only one filter is in use, it is selected
void FilterEffectsDialog::FilterModifier::update_selection(Selection *sel)
{
    std::set<SPObject*> used;

    for(GSList const *i = sel->itemList(); i != NULL; i = i->next) {
        SPObject *obj = SP_OBJECT (i->data);
        SPStyle *style = SP_OBJECT_STYLE (obj);
        if(!style || !SP_IS_ITEM(obj)) continue;

        if(style->filter.set && style->getFilter())
            used.insert(style->getFilter());
        else
            used.insert(0);
    }

    const int size = used.size();

    for(Gtk::TreeIter iter = _model->children().begin();
        iter != _model->children().end(); ++iter) {
        if(used.find((*iter)[_columns.filter]) != used.end()) {
            // If only one filter is in use by the selection, select it
            if(size == 1)
                _list.get_selection()->select(iter);
            (*iter)[_columns.sel] = size;
        }
        else
            (*iter)[_columns.sel] = 0;
    }
}

void FilterEffectsDialog::FilterModifier::on_filter_selection_changed()
{
    _observer->set(get_selected_filter());
    signal_filter_changed()();
}

void FilterEffectsDialog::FilterModifier::on_name_edited(const Glib::ustring& path, const Glib::ustring& text)
{
    Gtk::TreeModel::iterator iter = _model->get_iter(path);
    
    if(iter) {
        SPFilter* filter = (*iter)[_columns.filter];
        filter->setLabel(text.c_str());
        sp_document_done(filter->document, SP_VERB_DIALOG_FILTER_EFFECTS, _("Rename filter"));
        if(iter)
            (*iter)[_columns.label] = text;
    }
}

void FilterEffectsDialog::FilterModifier::on_selection_toggled(const Glib::ustring& path)
{
    Gtk::TreeIter iter = _model->get_iter(path);

    if(iter) {
        SPDesktop *desktop = _dialog.getDesktop();
        SPDocument *doc = sp_desktop_document(desktop);
        SPFilter* filter = (*iter)[_columns.filter];
        Inkscape::Selection *sel = sp_desktop_selection(desktop);

        /* If this filter is the only one used in the selection, unset it */
        if((*iter)[_columns.sel] == 1)
            filter = 0;

        GSList const *items = sel->itemList();
            
        for (GSList const *i = items; i != NULL; i = i->next) {
            SPItem * item = SP_ITEM(i->data);
            SPStyle *style = SP_OBJECT_STYLE(item);
            g_assert(style != NULL);
                
            if(filter)
                sp_style_set_property_url(SP_OBJECT(item), "filter", SP_OBJECT(filter), false);
            else
                ::remove_filter(item, false);

            SP_OBJECT(item)->requestDisplayUpdate((SP_OBJECT_MODIFIED_FLAG | SP_OBJECT_STYLE_MODIFIED_FLAG ));
        }
    
        update_selection(sel);
        sp_document_done(doc, SP_VERB_DIALOG_FILTER_EFFECTS,  _("Apply filter"));
    }
}

/* Add all filters in the document to the combobox.
   Keeps the same selection if possible, otherwise selects the first element */
void FilterEffectsDialog::FilterModifier::update_filters()
{
    SPDesktop* desktop = _dialog.getDesktop();
    SPDocument* document = sp_desktop_document(desktop);
    const GSList* filters = sp_document_get_resource_list(document, "filter");

    _model->clear();

    for(const GSList *l = filters; l; l = l->next) {
        Gtk::TreeModel::Row row = *_model->append();
        SPFilter* f = (SPFilter*)l->data;
        row[_columns.filter] = f;
        const gchar* lbl = f->label();
        const gchar* id = SP_OBJECT_ID(f);
        row[_columns.label] = lbl ? lbl : (id ? id : "filter");
    }

    update_selection(desktop->selection);
}

SPFilter* FilterEffectsDialog::FilterModifier::get_selected_filter()
{
    if(_list.get_selection()) {
        Gtk::TreeModel::iterator i = _list.get_selection()->get_selected();

        if(i)
            return (*i)[_columns.filter];
    }

    return 0;
}

void FilterEffectsDialog::FilterModifier::select_filter(const SPFilter* filter)
{
    if(filter) {
        for(Gtk::TreeModel::iterator i = _model->children().begin();
            i != _model->children().end(); ++i) {
            if((*i)[_columns.filter] == filter) {
                _list.get_selection()->select(i);
                break;
            }
        }
    }
}

void FilterEffectsDialog::FilterModifier::filter_list_button_release(GdkEventButton* event)
{
    if((event->type == GDK_BUTTON_RELEASE) && (event->button == 3)) {
        const bool sensitive = get_selected_filter() != NULL;
        _menu->items()[0].set_sensitive(sensitive);
        _menu->items()[1].set_sensitive(sensitive);
        _menu->popup(event->button, event->time);
    }
}

void FilterEffectsDialog::FilterModifier::add_filter()
{
    SPDocument* doc = sp_desktop_document(_dialog.getDesktop());
    SPFilter* filter = new_filter(doc);

    const int count = _model->children().size();
    std::ostringstream os;
    os << "filter" << count;
    filter->setLabel(os.str().c_str());

    update_filters();

    select_filter(filter);

    sp_document_done(doc, SP_VERB_DIALOG_FILTER_EFFECTS, _("Add filter"));
}

void FilterEffectsDialog::FilterModifier::remove_filter()
{
    SPFilter *filter = get_selected_filter();

    if(filter) {
        SPDocument* doc = filter->document;
        sp_repr_unparent(filter->repr);

        sp_document_done(doc, SP_VERB_DIALOG_FILTER_EFFECTS, _("Remove filter"));

        update_filters();
    }
}

void FilterEffectsDialog::FilterModifier::duplicate_filter()
{
    SPFilter* filter = get_selected_filter();

    if(filter) {
        Inkscape::XML::Node* repr = SP_OBJECT_REPR(filter), *parent = repr->parent();
        repr = repr->duplicate(repr->document());
        parent->appendChild(repr);

        sp_document_done(filter->document, SP_VERB_DIALOG_FILTER_EFFECTS, _("Duplicate filter"));

        update_filters();
    }
}

void FilterEffectsDialog::FilterModifier::rename_filter()
{
    _list.set_cursor(_model->get_path(_list.get_selection()->get_selected()), *_list.get_column(1), true);
}

FilterEffectsDialog::CellRendererConnection::CellRendererConnection()
    : Glib::ObjectBase(typeid(CellRendererConnection)),
      _primitive(*this, "primitive", 0)
{}

Glib::PropertyProxy<void*> FilterEffectsDialog::CellRendererConnection::property_primitive()
{
    return _primitive.get_proxy();
}

void FilterEffectsDialog::CellRendererConnection::set_text_width(const int w)
{
    _text_width = w;
}

int FilterEffectsDialog::CellRendererConnection::get_text_width() const
{
    return _text_width;
}

void FilterEffectsDialog::CellRendererConnection::get_size_vfunc(
    Gtk::Widget& widget, const Gdk::Rectangle* /*cell_area*/,
    int* x_offset, int* y_offset, int* width, int* height) const
{
    PrimitiveList& primlist = dynamic_cast<PrimitiveList&>(widget);

    if(x_offset)
        (*x_offset) = 0;
    if(y_offset)
        (*y_offset) = 0;
    if(width)
        (*width) = size * primlist.primitive_count() + _text_width * 7;
    if(height) {
        // Scale the height depending on the number of inputs, unless it's
        // the first primitive, in which case there are no connections
        SPFilterPrimitive* prim = (SPFilterPrimitive*)_primitive.get_value();
        (*height) = size * input_count(prim);
    }
}

/*** PrimitiveList ***/
FilterEffectsDialog::PrimitiveList::PrimitiveList(FilterEffectsDialog& d)
    : _dialog(d),
      _in_drag(0),
      _observer(new SignalObserver)
{
    d.signal_expose_event().connect(sigc::mem_fun(*this, &PrimitiveList::on_expose_signal));
    
    add_events(Gdk::POINTER_MOTION_MASK | Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK);
    signal_expose_event().connect(sigc::mem_fun(*this, &PrimitiveList::on_expose_signal));

    _model = Gtk::ListStore::create(_columns);

    set_reorderable(true);

    set_model(_model);
    append_column(_("_Effect"), _columns.type);

    _observer->signal_changed().connect(signal_primitive_changed().make_slot());
    get_selection()->signal_changed().connect(sigc::mem_fun(*this, &PrimitiveList::on_primitive_selection_changed));
    signal_primitive_changed().connect(sigc::mem_fun(*this, &PrimitiveList::queue_draw));

    _connection_cell.set_text_width(init_text());

    int cols_count = append_column(_("Connections"), _connection_cell);
    Gtk::TreeViewColumn* col = get_column(cols_count - 1);
    if(col)
       col->add_attribute(_connection_cell.property_primitive(), _columns.primitive);
}

// Sets up a vertical Pango context/layout, and returns the largest
// width needed to render the FilterPrimitiveInput labels.
int FilterEffectsDialog::PrimitiveList::init_text()
{
    // Set up a vertical context+layout
    Glib::RefPtr<Pango::Context> context = create_pango_context();
    const Pango::Matrix matrix = {0, -1, 1, 0, 0, 0};
    context->set_matrix(matrix);
    _vertical_layout = Pango::Layout::create(context);

    int maxfont = 0;
    for(int i = 0; i < FPInputConverter.end; ++i) {
        _vertical_layout->set_text(_(FPInputConverter.get_label((FilterPrimitiveInput)i).c_str()));
        int fontw, fonth;
        _vertical_layout->get_pixel_size(fontw, fonth);
        if(fonth > maxfont)
            maxfont = fonth;
    }

    return maxfont;
}

sigc::signal<void>& FilterEffectsDialog::PrimitiveList::signal_primitive_changed()
{
    return _signal_primitive_changed;
}

void FilterEffectsDialog::PrimitiveList::on_primitive_selection_changed()
{
    _observer->set(get_selected());
    signal_primitive_changed()();
    _dialog._color_matrix_values->clear_store();
}

/* Add all filter primitives in the current to the list.
   Keeps the same selection if possible, otherwise selects the first element */
void FilterEffectsDialog::PrimitiveList::update()
{
    SPFilter* f = _dialog._filter_modifier.get_selected_filter();
    const SPFilterPrimitive* active_prim = get_selected();
    bool active_found = false;

    _model->clear();

    if(f) {
        _dialog._primitive_box.set_sensitive(true);

        for(SPObject *prim_obj = f->children;
                prim_obj && SP_IS_FILTER_PRIMITIVE(prim_obj);
                prim_obj = prim_obj->next) {
            SPFilterPrimitive *prim = SP_FILTER_PRIMITIVE(prim_obj);
            if(prim) {
                Gtk::TreeModel::Row row = *_model->append();
                row[_columns.primitive] = prim;
                row[_columns.type_id] = FPConverter.get_id_from_key(prim->repr->name());
                row[_columns.type] = _(FPConverter.get_label(row[_columns.type_id]).c_str());
                row[_columns.id] = SP_OBJECT_ID(prim);

                if(prim == active_prim) {
                    get_selection()->select(row);
                    active_found = true;
                }
            }
        }

        if(!active_found && _model->children().begin())
            get_selection()->select(_model->children().begin());

        columns_autosize();
    }
    else {
        _dialog._primitive_box.set_sensitive(false);
    }
}

void FilterEffectsDialog::PrimitiveList::set_menu(Glib::RefPtr<Gtk::Menu> menu)
{
    _primitive_menu = menu;
}

SPFilterPrimitive* FilterEffectsDialog::PrimitiveList::get_selected()
{
    if(_dialog._filter_modifier.get_selected_filter()) {
        Gtk::TreeModel::iterator i = get_selection()->get_selected();
        if(i)
            return (*i)[_columns.primitive];
    }

    return 0;
}

void FilterEffectsDialog::PrimitiveList::select(SPFilterPrimitive* prim)
{
    for(Gtk::TreeIter i = _model->children().begin();
        i != _model->children().end(); ++i) {
        if((*i)[_columns.primitive] == prim)
            get_selection()->select(i);
    }
}

void FilterEffectsDialog::PrimitiveList::remove_selected()
{
    SPFilterPrimitive* prim = get_selected();

    if(prim) {
        _observer->set(0);

        sp_repr_unparent(prim->repr);

        sp_document_done(sp_desktop_document(_dialog.getDesktop()), SP_VERB_DIALOG_FILTER_EFFECTS,
                         _("Remove filter primitive"));

        update();
    }
}

bool FilterEffectsDialog::PrimitiveList::on_expose_signal(GdkEventExpose* e)
{
    Gdk::Rectangle clip(e->area.x, e->area.y, e->area.width, e->area.height);
    Glib::RefPtr<Gdk::Window> win = get_bin_window();
    Glib::RefPtr<Gdk::GC> darkgc = get_style()->get_dark_gc(Gtk::STATE_NORMAL);

    SPFilterPrimitive* prim = get_selected();
    int row_count = get_model()->children().size();

    int fheight = CellRendererConnection::size;
    Gdk::Rectangle rct, vis;
    Gtk::TreeIter row = get_model()->children().begin();
    int text_start_x = 0;
    if(row) {
        get_cell_area(get_model()->get_path(row), *get_column(1), rct);
        get_visible_rect(vis);
        int vis_x, vis_y;
        tree_to_widget_coords(vis.get_x(), vis.get_y(), vis_x, vis_y);

        text_start_x = rct.get_x() + rct.get_width() - _connection_cell.get_text_width() * (FPInputConverter.end + 1) + 1;
        for(int i = 0; i < FPInputConverter.end; ++i) {
            _vertical_layout->set_text(_(FPInputConverter.get_label((FilterPrimitiveInput)i).c_str()));
            const int x = text_start_x + _connection_cell.get_text_width() * (i + 1);
            get_bin_window()->draw_rectangle(get_style()->get_bg_gc(Gtk::STATE_NORMAL), true, x, vis_y, _connection_cell.get_text_width(), vis.get_height());
            get_bin_window()->draw_layout(get_style()->get_text_gc(Gtk::STATE_NORMAL), x + 1, vis_y, _vertical_layout);
            get_bin_window()->draw_line(darkgc, x, vis_y, x, vis_y + vis.get_height());
        }
    }

    int row_index = 0;
    for(; row != get_model()->children().end(); ++row, ++row_index) {
        get_cell_area(get_model()->get_path(row), *get_column(1), rct);
        const int x = rct.get_x(), y = rct.get_y(), h = rct.get_height();

        // Check mouse state
        int mx, my;
        Gdk::ModifierType mask;
        get_bin_window()->get_pointer(mx, my, mask);

        // Outline the bottom of the connection area
        const int outline_x = x + fheight * (row_count - row_index);
        get_bin_window()->draw_line(darkgc, x, y + h, outline_x, y + h);

        // Side outline
        get_bin_window()->draw_line(darkgc, outline_x, y - 1, outline_x, y + h);

        std::vector<Gdk::Point> con_poly;
        int con_drag_y;
        bool inside;
        const SPFilterPrimitive* row_prim = (*row)[_columns.primitive];
        const int inputs = input_count(row_prim);

        if(SP_IS_FEMERGE(row_prim)) {
            for(int i = 0; i < inputs; ++i) {
                inside = do_connection_node(row, i, con_poly, mx, my);
                get_bin_window()->draw_polygon(inside && mask & GDK_BUTTON1_MASK ?
                                               darkgc : get_style()->get_dark_gc(Gtk::STATE_ACTIVE),
                                               inside, con_poly);

                if(_in_drag == (i + 1))
                    con_drag_y = con_poly[2].get_y();

                if(_in_drag != (i + 1) || row_prim != prim)
                    draw_connection(row, i, text_start_x, outline_x, con_poly[2].get_y(), row_count);
            }
        }
        else {
            // Draw "in" shape
            inside = do_connection_node(row, 0, con_poly, mx, my);
            con_drag_y = con_poly[2].get_y();
            get_bin_window()->draw_polygon(inside && mask & GDK_BUTTON1_MASK ?
                                           darkgc : get_style()->get_dark_gc(Gtk::STATE_ACTIVE),
                                           inside, con_poly);

            // Draw "in" connection
            if(_in_drag != 1 || row_prim != prim)
                draw_connection(row, SP_ATTR_IN, text_start_x, outline_x, con_poly[2].get_y(), row_count);

            if(inputs == 2) {
                // Draw "in2" shape
                inside = do_connection_node(row, 1, con_poly, mx, my);
                if(_in_drag == 2)
                    con_drag_y = con_poly[2].get_y();
                get_bin_window()->draw_polygon(inside && mask & GDK_BUTTON1_MASK ?
                                               darkgc : get_style()->get_dark_gc(Gtk::STATE_ACTIVE),
                                               inside, con_poly);
                // Draw "in2" connection
                if(_in_drag != 2 || row_prim != prim)
                    draw_connection(row, SP_ATTR_IN2, text_start_x, outline_x, con_poly[2].get_y(), row_count);
            }
        }

        // Draw drag connection
        if(row_prim == prim && _in_drag) {
            get_bin_window()->draw_line(get_style()->get_black_gc(), outline_x, con_drag_y,
                                        mx, con_drag_y);
            get_bin_window()->draw_line(get_style()->get_black_gc(), mx, con_drag_y, mx, my);
        }
    }

    return true;
}

void FilterEffectsDialog::PrimitiveList::draw_connection(const Gtk::TreeIter& input, const int attr,
                                                         const int text_start_x, const int x1, const int y1,
                                                         const int row_count)
{
    int src_id = 0;
    Gtk::TreeIter res = find_result(input, attr, src_id);
    Glib::RefPtr<Gdk::GC> darkgc = get_style()->get_black_gc();
    Glib::RefPtr<Gdk::GC> lightgc = get_style()->get_dark_gc(Gtk::STATE_NORMAL);
    Glib::RefPtr<Gdk::GC> gc;

    const bool is_first = input == get_model()->children().begin();
    const bool is_merge = SP_IS_FEMERGE((SPFilterPrimitive*)(*input)[_columns.primitive]);
    const bool use_default = !res && !is_merge;
    
    if(res == input || (use_default && is_first)) {
        // Draw straight connection to a standard input
        // Draw a lighter line for an implicit connection to a standard input
        const int tw = _connection_cell.get_text_width();
        gint end_x = text_start_x + tw * (src_id + 1) + (int)(tw * 0.5f) + 1;
        gc = (use_default && is_first) ? lightgc : darkgc;
        get_bin_window()->draw_rectangle(gc, true, end_x-2, y1-2, 5, 5);
        get_bin_window()->draw_line(gc, x1, y1, end_x, y1);
    }
    else {
        // Draw an 'L'-shaped connection to another filter primitive
        // If no connection is specified, draw a light connection to the previous primitive
        gc = use_default ? lightgc : darkgc;

        if(use_default) {
            res = input;
            --res;
        }

        if(res) {
            Gdk::Rectangle rct;
            
            get_cell_area(get_model()->get_path(_model->children().begin()), *get_column(1), rct);
            const int fheight = CellRendererConnection::size;
            
            get_cell_area(get_model()->get_path(res), *get_column(1), rct);
            const int row_index = find_index(res);
            const int x2 = rct.get_x() + fheight * (row_count - row_index) - fheight / 2;
            const int y2 = rct.get_y() + rct.get_height();

            // Draw a bevelled 'L'-shaped connection
            get_bin_window()->draw_line(get_style()->get_black_gc(), x1, y1, x2-fheight/4, y1);
            get_bin_window()->draw_line(get_style()->get_black_gc(), x2-fheight/4, y1, x2, y1-fheight/4);
            get_bin_window()->draw_line(get_style()->get_black_gc(), x2, y1-fheight/4, x2, y2);            
        }
    }
}

// Creates a triangle outline of the connection node and returns true if (x,y) is inside the node
bool FilterEffectsDialog::PrimitiveList::do_connection_node(const Gtk::TreeIter& row, const int input,
                                                            std::vector<Gdk::Point>& points,
                                                            const int ix, const int iy)
{
    Gdk::Rectangle rct;
    const int icnt = input_count((*row)[_columns.primitive]);

    get_cell_area(get_model()->get_path(_model->children().begin()), *get_column(1), rct);
    const int fheight = CellRendererConnection::size;

    get_cell_area(_model->get_path(row), *get_column(1), rct);
    const float h = rct.get_height() / icnt;

    const int x = rct.get_x() + fheight * (_model->children().size() - find_index(row));
    const int con_w = (int)(fheight * 0.35f);
    const int con_y = (int)(rct.get_y() + (h / 2) - con_w + (input * h));
    points.clear();
    points.push_back(Gdk::Point(x, con_y));
    points.push_back(Gdk::Point(x, con_y + con_w * 2));
    points.push_back(Gdk::Point(x - con_w, con_y + con_w));

    return ix >= x - h && iy >= con_y && ix <= x && iy <= points[1].get_y();
}

const Gtk::TreeIter FilterEffectsDialog::PrimitiveList::find_result(const Gtk::TreeIter& start,
                                                                    const int attr, int& src_id)
{
    SPFilterPrimitive* prim = (*start)[_columns.primitive];
    Gtk::TreeIter target = _model->children().end();
    int image;

    if(SP_IS_FEMERGE(prim)) {
        int c = 0;
        bool found = false;
        for(const SPObject* o = prim->firstChild(); o; o = o->next, ++c) {
            if(c == attr && SP_IS_FEMERGENODE(o)) {
                image = SP_FEMERGENODE(o)->input;
                found = true;
            }
        }
        if(!found)
            return target;
    }
    else {
        if(attr == SP_ATTR_IN)
            image = prim->image_in;
        else if(attr == SP_ATTR_IN2) {
            if(SP_IS_FEBLEND(prim))
                image = SP_FEBLEND(prim)->in2;
            else if(SP_IS_FECOMPOSITE(prim))
                image = SP_FECOMPOSITE(prim)->in2;
            else if(SP_IS_FEDISPLACEMENTMAP(prim))
                image = SP_FEDISPLACEMENTMAP(prim)->in2;
            else
                return target;
        }
        else
            return target;
    }

    if(image >= 0) {
        for(Gtk::TreeIter i = _model->children().begin();
            i != start; ++i) {
            if(((SPFilterPrimitive*)(*i)[_columns.primitive])->image_out == image)
                target = i;
        }
        return target;
    }
    else if(image < -1) {
        src_id = -(image + 2);
        return start;
    }

    return target;
}

int FilterEffectsDialog::PrimitiveList::find_index(const Gtk::TreeIter& target)
{
    int i = 0;
    for(Gtk::TreeIter iter = _model->children().begin();
        iter != target; ++iter, ++i);
    return i;
}

bool FilterEffectsDialog::PrimitiveList::on_button_press_event(GdkEventButton* e)
{
    Gtk::TreePath path;
    Gtk::TreeViewColumn* col;
    const int x = (int)e->x, y = (int)e->y;
    int cx, cy;

    _drag_prim = 0;
    
    if(get_path_at_pos(x, y, path, col, cx, cy)) {
        Gtk::TreeIter iter = _model->get_iter(path);
        std::vector<Gdk::Point> points;

        _drag_prim = (*iter)[_columns.primitive];
        const int icnt = input_count(_drag_prim);

        for(int i = 0; i < icnt; ++i) {
            if(do_connection_node(_model->get_iter(path), i, points, x, y)) {
                _in_drag = i + 1;
                break;
            }
        }
        
        queue_draw();
    }

    if(_in_drag) {
        _scroll_connection = Glib::signal_timeout().connect(sigc::mem_fun(*this, &PrimitiveList::on_scroll_timeout), 150);
        _autoscroll = 0;
        get_selection()->select(path);
        return true;
    }
    else
        return Gtk::TreeView::on_button_press_event(e);
}

bool FilterEffectsDialog::PrimitiveList::on_motion_notify_event(GdkEventMotion* e)
{
    const int speed = 10;
    const int limit = 15;

    Gdk::Rectangle vis;
    get_visible_rect(vis);
    int vis_x, vis_y;
    tree_to_widget_coords(vis.get_x(), vis.get_y(), vis_x, vis_y);
    const int top = vis_y + vis.get_height();

    // When autoscrolling during a connection drag, set the speed based on
    // where the mouse is in relation to the edges.
    if(e->y < vis_y)
        _autoscroll = -(int)(speed + (vis_y - e->y) / 5);
    else if(e->y < vis_y + limit)
        _autoscroll = -speed;
    else if(e->y > top)
        _autoscroll = (int)(speed + (e->y - top) / 5);
    else if(e->y > top - limit)
        _autoscroll = speed;
    else
        _autoscroll = 0;

    queue_draw();

    return Gtk::TreeView::on_motion_notify_event(e);
}

bool FilterEffectsDialog::PrimitiveList::on_button_release_event(GdkEventButton* e)
{
    SPFilterPrimitive *prim = get_selected(), *target;

    _scroll_connection.disconnect();

    if(_in_drag && prim) {
        Gtk::TreePath path;
        Gtk::TreeViewColumn* col;
        int cx, cy;
        
        if(get_path_at_pos((int)e->x, (int)e->y, path, col, cx, cy)) {
            const gchar *in_val = 0;
            Glib::ustring result;
            Gtk::TreeIter target_iter = _model->get_iter(path);
            target = (*target_iter)[_columns.primitive];

            Gdk::Rectangle rct;
            get_cell_area(path, *col, rct);
            const int twidth = _connection_cell.get_text_width();
            const int sources_x = rct.get_width() - twidth * FPInputConverter.end;
            if(cx > sources_x) {
                int src = (cx - sources_x) / twidth;
                if(src < 0)
                    src = 0;
                else if(src >= FPInputConverter.end)
                    src = FPInputConverter.end - 1;
                result = FPInputConverter.get_key((FilterPrimitiveInput)src);
                in_val = result.c_str();
            }
            else {
                // Ensure that the target comes before the selected primitive
                for(Gtk::TreeIter iter = _model->children().begin();
                    iter != get_selection()->get_selected(); ++iter) {
                    if(iter == target_iter) {
                        Inkscape::XML::Node *repr = SP_OBJECT_REPR(target);
                        // Make sure the target has a result
                        const gchar *gres = repr->attribute("result");
                        if(!gres) {
                            result = "result" + Glib::Ascii::dtostr(SP_FILTER(prim->parent)->_image_number_next);
                            repr->setAttribute("result", result.c_str());
                            in_val = result.c_str();
                        }
                        else
                            in_val = gres;
                        break;
                    }
                }
            }

            if(SP_IS_FEMERGE(prim)) {
                int c = 1;
                bool handled = false;
                for(SPObject* o = prim->firstChild(); o && !handled; o = o->next, ++c) {
                    if(c == _in_drag && SP_IS_FEMERGENODE(o)) {
                        // If input is null, delete it
                        if(!in_val) {
                            sp_repr_unparent(o->repr);
                            sp_document_done(prim->document, SP_VERB_DIALOG_FILTER_EFFECTS,
                                             _("Remove merge node"));
                            (*get_selection()->get_selected())[_columns.primitive] = prim;
                        }
                        else
                            _dialog.set_attr(o, SP_ATTR_IN, in_val);
                        handled = true;
                    }
                }
                // Add new input?
                if(!handled && c == _in_drag && in_val) {
                    Inkscape::XML::Document *xml_doc = sp_document_repr_doc(prim->document);
                    Inkscape::XML::Node *repr = xml_doc->createElement("svg:feMergeNode");
                    repr->setAttribute("inkscape:collect", "always");
                    prim->repr->appendChild(repr);
                    SPFeMergeNode *node = SP_FEMERGENODE(prim->document->getObjectByRepr(repr));
                    Inkscape::GC::release(repr);
                    _dialog.set_attr(node, SP_ATTR_IN, in_val);
                    (*get_selection()->get_selected())[_columns.primitive] = prim;
                }
            }
            else {
                if(_in_drag == 1)
                    _dialog.set_attr(prim, SP_ATTR_IN, in_val);
                else if(_in_drag == 2)
                    _dialog.set_attr(prim, SP_ATTR_IN2, in_val);
            }
        }

        _in_drag = 0;
        queue_draw();

        _dialog.update_settings_view();
    }

    if((e->type == GDK_BUTTON_RELEASE) && (e->button == 3)) {
        const bool sensitive = get_selected() != NULL;
        _primitive_menu->items()[0].set_sensitive(sensitive);
        _primitive_menu->items()[1].set_sensitive(sensitive);
        _primitive_menu->popup(e->button, e->time);

        return true;
    }
    else
        return Gtk::TreeView::on_button_release_event(e);
}

// Checks all of prim's inputs, removes any that use result
void check_single_connection(SPFilterPrimitive* prim, const int result)
{
    if(prim && result >= 0) {

        if(prim->image_in == result)
            SP_OBJECT_REPR(prim)->setAttribute("in", 0);

        if(SP_IS_FEBLEND(prim)) {
            if(SP_FEBLEND(prim)->in2 == result)
                SP_OBJECT_REPR(prim)->setAttribute("in2", 0);
        }
        else if(SP_IS_FECOMPOSITE(prim)) {
            if(SP_FECOMPOSITE(prim)->in2 == result)
                SP_OBJECT_REPR(prim)->setAttribute("in2", 0);
        }
        else if(SP_IS_FEDISPLACEMENTMAP(prim)) {
            if(SP_FEDISPLACEMENTMAP(prim)->in2 == result)
                SP_OBJECT_REPR(prim)->setAttribute("in2", 0);
        }
    }
}

// Remove any connections going to/from prim_iter that forward-reference other primitives
void FilterEffectsDialog::PrimitiveList::sanitize_connections(const Gtk::TreeIter& prim_iter)
{
    SPFilterPrimitive *prim = (*prim_iter)[_columns.primitive];
    bool before = true;

    for(Gtk::TreeIter iter = _model->children().begin();
        iter != _model->children().end(); ++iter) {
        if(iter == prim_iter)
            before = false;
        else {
            SPFilterPrimitive* cur_prim = (*iter)[_columns.primitive];
            if(before)
                check_single_connection(cur_prim, prim->image_out);
            else
                check_single_connection(prim, cur_prim->image_out);
        }
    }
}

// Reorder the filter primitives to match the list order
void FilterEffectsDialog::PrimitiveList::on_drag_end(const Glib::RefPtr<Gdk::DragContext>& /*dc*/)
{
    SPFilter* filter = _dialog._filter_modifier.get_selected_filter();
    int ndx = 0;

    for(Gtk::TreeModel::iterator iter = _model->children().begin();
        iter != _model->children().end(); ++iter, ++ndx) {
        SPFilterPrimitive* prim = (*iter)[_columns.primitive];
        if(prim && prim == _drag_prim) {
            SP_OBJECT_REPR(prim)->setPosition(ndx);
            break;
        }
    }

    for(Gtk::TreeModel::iterator iter = _model->children().begin();
        iter != _model->children().end(); ++iter, ++ndx) {
        SPFilterPrimitive* prim = (*iter)[_columns.primitive];
        if(prim && prim == _drag_prim) {
            sanitize_connections(iter);
            get_selection()->select(iter);
            break;
        }
    }

    filter->requestModified(SP_OBJECT_MODIFIED_FLAG);

    sp_document_done(filter->document, SP_VERB_DIALOG_FILTER_EFFECTS, _("Reorder filter primitive"));
}

// If a connection is dragged towards the top or bottom of the list, the list should scroll to follow.
bool FilterEffectsDialog::PrimitiveList::on_scroll_timeout()
{
    if(_autoscroll) {
        Gtk::Adjustment& a = *dynamic_cast<Gtk::ScrolledWindow*>(get_parent())->get_vadjustment();
        double v;

        v = a.get_value() + _autoscroll;
        if(v < 0)
            v = 0;
        if(v > a.get_upper() - a.get_page_size())
            v = a.get_upper() - a.get_page_size();

        a.set_value(v);

        queue_draw();
    }

    return true;
}

int FilterEffectsDialog::PrimitiveList::primitive_count() const
{
    return _model->children().size();
}

/*** FilterEffectsDialog ***/

FilterEffectsDialog::FilterEffectsDialog() 
    : UI::Widget::Panel("", "dialogs.filtereffects", SP_VERB_DIALOG_FILTER_EFFECTS),
      _filter_modifier(*this),
      _primitive_list(*this),
      _add_primitive_type(FPConverter),
      _add_primitive(_("Add Effect:")),
      _empty_settings(_("No effect selected"), Gtk::ALIGN_LEFT),
      _locked(false),
      _attr_lock(false)
{
    _settings = new Settings(*this, _settings_box, sigc::mem_fun(*this, &FilterEffectsDialog::set_attr_direct),
                             NR_FILTER_ENDPRIMITIVETYPE);
    _sizegroup = Gtk::SizeGroup::create(Gtk::SIZE_GROUP_HORIZONTAL);
    _sizegroup->set_ignore_hidden();

    _add_primitive_type.remove_row(NR_FILTER_IMAGE);
    _add_primitive_type.remove_row(NR_FILTER_TILE);
    _add_primitive_type.remove_row(NR_FILTER_COMPONENTTRANSFER);
        
    // Initialize widget hierarchy
    Gtk::HPaned* hpaned = Gtk::manage(new Gtk::HPaned);
    Gtk::ScrolledWindow* sw_prims = Gtk::manage(new Gtk::ScrolledWindow);
    Gtk::HBox* infobox = Gtk::manage(new Gtk::HBox);
    Gtk::HBox* hb_prims = Gtk::manage(new Gtk::HBox);
    Gtk::Frame* fr_settings = Gtk::manage(new Gtk::Frame(_("<b>Effect parameters</b>")));
    Gtk::Alignment* al_settings = Gtk::manage(new Gtk::Alignment);
    _getContents()->add(*hpaned);
    hpaned->pack1(_filter_modifier);
    hpaned->pack2(_primitive_box);
    _primitive_box.pack_start(*sw_prims);
    _primitive_box.pack_start(*infobox,false, false);    
    _primitive_box.pack_start(*hb_prims, false, false);
    sw_prims->add(_primitive_list);
    infobox->pack_start(_infobox_icon, false, false);
    infobox->pack_end(_infobox_desc, false, false);
    _infobox_desc.set_line_wrap(true);
    
    hb_prims->pack_end(_add_primitive_type, false, false);
    hb_prims->pack_end(_add_primitive, false, false);
    _getContents()->pack_start(*fr_settings, false, false);
    fr_settings->add(*al_settings);
    al_settings->add(_settings_box);
    
    _primitive_list.signal_primitive_changed().connect(
        sigc::mem_fun(*this, &FilterEffectsDialog::update_settings_view));
    _filter_modifier.signal_filter_changed().connect(
        sigc::mem_fun(_primitive_list, &PrimitiveList::update));

    _add_primitive_type.signal_changed().connect(
        sigc::mem_fun(*this, &FilterEffectsDialog::update_primitive_infobox));
          
    sw_prims->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    sw_prims->set_shadow_type(Gtk::SHADOW_IN);
    al_settings->set_padding(0, 0, 12, 0);
    fr_settings->set_shadow_type(Gtk::SHADOW_NONE);
    ((Gtk::Label*)fr_settings->get_label_widget())->set_use_markup();
    _add_primitive.signal_clicked().connect(sigc::mem_fun(*this, &FilterEffectsDialog::add_primitive));
    _primitive_list.set_menu(create_popup_menu(*this, sigc::mem_fun(*this, &FilterEffectsDialog::duplicate_primitive),
                                               sigc::mem_fun(_primitive_list, &PrimitiveList::remove_selected)));
    
    show_all_children();
    init_settings_widgets();
    _primitive_list.update();
    update_primitive_infobox();
}

FilterEffectsDialog::~FilterEffectsDialog()
{
    delete _settings;
}

void FilterEffectsDialog::set_attrs_locked(const bool l)
{
    _locked = l;
}

void FilterEffectsDialog::show_all_vfunc()
{
    UI::Widget::Panel::show_all_vfunc();

    update_settings_view();
}

void FilterEffectsDialog::init_settings_widgets()
{
    // TODO: Find better range/climb-rate/digits values for the SpinSliders,
    //       most of the current values are complete guesses!

    _empty_settings.set_sensitive(false);
    _settings_box.pack_start(_empty_settings);

    _settings->type(NR_FILTER_BLEND);
    _settings->add_combo(SP_ATTR_MODE, _("Mode"), BlendModeConverter);

    _settings->type(NR_FILTER_COLORMATRIX);
    ComboBoxEnum<FilterColorMatrixType>* colmat = _settings->add_combo(SP_ATTR_TYPE, _("Type"), ColorMatrixTypeConverter);
    _color_matrix_values = _settings->add_colormatrixvalues(_("Value(s)"));
    colmat->signal_attr_changed().connect(sigc::mem_fun(*this, &FilterEffectsDialog::update_color_matrix));

    _settings->type(NR_FILTER_COMPONENTTRANSFER);
    _settings->add_notimplemented();
    /*_settings->add_combo(SP_ATTR_TYPE, _("Type"), ComponentTransferTypeConverter);
    _ct_slope = _settings->add_spinslider(SP_ATTR_SLOPE, _("Slope"), -100, 100, 1, 0.01, 1);
    _ct_intercept = _settings->add_spinslider(SP_ATTR_INTERCEPT, _("Intercept"), -100, 100, 1, 0.01, 1);
    _ct_amplitude = _settings->add_spinslider(SP_ATTR_AMPLITUDE, _("Amplitude"), 0, 100, 1, 0.01, 1);
    _ct_exponent = _settings->add_spinslider(SP_ATTR_EXPONENT, _("Exponent"), 0, 100, 1, 0.01, 1);
    _ct_offset = _settings->add_spinslider(SP_ATTR_OFFSET, _("Offset"), -100, 100, 1, 0.01, 1);*/

    _settings->type(NR_FILTER_COMPOSITE);
    _settings->add_combo(SP_ATTR_OPERATOR, _("Operator"), CompositeOperatorConverter);
    _k1 = _settings->add_spinslider(SP_ATTR_K1, _("K1"), -10, 10, 0.1, 0.01, 2);
    _k2 = _settings->add_spinslider(SP_ATTR_K2, _("K2"), -10, 10, 0.1, 0.01, 2);
    _k3 = _settings->add_spinslider(SP_ATTR_K3, _("K3"), -10, 10, 0.1, 0.01, 2);
    _k4 = _settings->add_spinslider(SP_ATTR_K4, _("K4"), -10, 10, 0.1, 0.01, 2);

    _settings->type(NR_FILTER_CONVOLVEMATRIX);
    _convolve_order = _settings->add_dualspinbutton(SP_ATTR_ORDER, _("Size"), 1, 5, 1, 1, 0);
    _convolve_target = _settings->add_multispinbutton(SP_ATTR_TARGETX, SP_ATTR_TARGETY, _("Target"), 0, 4, 1, 1, 0);
    _convolve_matrix = _settings->add_matrix(SP_ATTR_KERNELMATRIX, _("Kernel"));
    _convolve_order->signal_attr_changed().connect(sigc::mem_fun(*this, &FilterEffectsDialog::convolve_order_changed));
    _settings->add_spinslider(SP_ATTR_DIVISOR, _("Divisor"), 1, 20, 1, 0.1, 2);
    _settings->add_spinslider(SP_ATTR_BIAS, _("Bias"), -10, 10, 1, 0.01, 1);
    _settings->add_combo(SP_ATTR_EDGEMODE, _("Edge Mode"), ConvolveMatrixEdgeModeConverter);
    _settings->add_checkbutton(SP_ATTR_PRESERVEALPHA, _("Preserve Alpha"), "true", "false");

    _settings->type(NR_FILTER_DIFFUSELIGHTING);
    _settings->add_color(SP_PROP_LIGHTING_COLOR, _("Diffuse Color"));
    _settings->add_spinslider(SP_ATTR_SURFACESCALE, _("Surface Scale"), -1000, 1000, 1, 0.01, 1);
    _settings->add_spinslider(SP_ATTR_DIFFUSECONSTANT, _("Constant"), 0, 100, 0.1, 0.01, 2);
    _settings->add_dualspinslider(SP_ATTR_KERNELUNITLENGTH, _("Kernel Unit Length"), 0.01, 10, 1, 0.01, 1);
    _settings->add_lightsource();

    _settings->type(NR_FILTER_DISPLACEMENTMAP);
    _settings->add_spinslider(SP_ATTR_SCALE, _("Scale"), 0, 100, 1, 0.01, 1);
    _settings->add_combo(SP_ATTR_XCHANNELSELECTOR, _("X Channel"), DisplacementMapChannelConverter);
    _settings->add_combo(SP_ATTR_YCHANNELSELECTOR, _("Y Channel"), DisplacementMapChannelConverter);

    _settings->type(NR_FILTER_FLOOD);
    _settings->add_color(SP_PROP_FLOOD_COLOR, _("Flood Color"));
    _settings->add_spinslider(SP_PROP_FLOOD_OPACITY, _("Opacity"), 0, 1, 0.1, 0.01, 2);
    
    _settings->type(NR_FILTER_GAUSSIANBLUR);
    _settings->add_dualspinslider(SP_ATTR_STDDEVIATION, _("Standard Deviation"), 0.01, 100, 1, 0.01, 1);

    _settings->type(NR_FILTER_MORPHOLOGY);
    _settings->add_combo(SP_ATTR_OPERATOR, _("Operator"), MorphologyOperatorConverter);
    _settings->add_dualspinslider(SP_ATTR_RADIUS, _("Radius"), 0, 100, 1, 0.01, 1);

    _settings->type(NR_FILTER_IMAGE);
    _settings->add_notimplemented();

    _settings->type(NR_FILTER_OFFSET);
    _settings->add_spinslider(SP_ATTR_DX, _("Delta X"), -100, 100, 1, 0.01, 1);
    _settings->add_spinslider(SP_ATTR_DY, _("Delta Y"), -100, 100, 1, 0.01, 1);

    _settings->type(NR_FILTER_SPECULARLIGHTING);
    _settings->add_color(SP_PROP_LIGHTING_COLOR, _("Specular Color"));
    _settings->add_spinslider(SP_ATTR_SURFACESCALE, _("Surface Scale"), -1000, 1000, 1, 0.01, 1);
    _settings->add_spinslider(SP_ATTR_SPECULARCONSTANT, _("Constant"), 0, 100, 0.1, 0.01, 2);
    _settings->add_spinslider(SP_ATTR_SPECULAREXPONENT, _("Exponent"), 1, 128, 1, 0.01, 1);
    _settings->add_dualspinslider(SP_ATTR_KERNELUNITLENGTH, _("Kernel Unit Length"), 0.01, 10, 1, 0.01, 1);
    _settings->add_lightsource();
    
    _settings->type(NR_FILTER_TILE);
    _settings->add_notimplemented();

    _settings->type(NR_FILTER_TURBULENCE);
    _settings->add_checkbutton(SP_ATTR_STITCHTILES, _("Stitch Tiles"), "stitch", "noStitch");
    _settings->add_combo(SP_ATTR_TYPE, _("Type"), TurbulenceTypeConverter);
    _settings->add_dualspinslider(SP_ATTR_BASEFREQUENCY, _("Base Frequency"), 0, 1, 0.001, 0.01, 3);
    _settings->add_spinslider(SP_ATTR_NUMOCTAVES, _("Octaves"), 1, 10, 1, 1, 0);
    _settings->add_spinslider(SP_ATTR_SEED, _("Seed"), 0, 1000, 1, 1, 0);
}

void FilterEffectsDialog::add_primitive()
{
    SPFilter* filter = _filter_modifier.get_selected_filter();

    if(filter) {
        SPFilterPrimitive* prim = filter_add_primitive(filter, _add_primitive_type.get_active_data()->id);

        _primitive_list.select(prim);

        sp_document_done(filter->document, SP_VERB_DIALOG_FILTER_EFFECTS, _("Add filter primitive"));
    }
}

void FilterEffectsDialog::update_primitive_infobox()
{
    switch(_add_primitive_type.get_active_data()->id){
        case(NR::NR_FILTER_BLEND):
            _infobox_icon.set(g_strdup_printf("%s/feBlend-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feBlend</b> filter primitive provides 4 image blending modes: screen, multiply, darken and lighten."));
            break;
        case(NR::NR_FILTER_COLORMATRIX):
            _infobox_icon.set(g_strdup_printf("%s/feColorMatrix-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feColorMatrix</b> filter primitive applies a matrix transformation to colour of each rendered pixel. This allows for effects like turning object to grayscale, modifying colour saturation and changing colour hue."));
            break;
        case(NR::NR_FILTER_COMPONENTTRANSFER):
            _infobox_icon.set(g_strdup_printf("%s/feComponentTransfer-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feComponentTransfer</b> filter primitive manipulates the input's color components (red, green, blue, and alpha) according to particular transfer functions, allowing operations like brightness and contrast adjustment, color balance, and thresholding."));
            break;
        case(NR::NR_FILTER_COMPOSITE):
            _infobox_icon.set(g_strdup_printf("%s/feComposite-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feComposite</b> filter primitive composites two images using one of the Porter-Duff blending modes or the aritmetic mode described in SVG standard. Porter-Duff blending modes are essentially logical operations between the corresponding pixel values of the images."));
            break;
        case(NR::NR_FILTER_CONVOLVEMATRIX):
            _infobox_icon.set(g_strdup_printf("%s/feConvolveMatrix-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feConvolveMatrix</b> lets you specify a Convolution to be applied on the image. Common effects created using convolution matrices are blur, sharpening, embossing and edge detection. Note that while gaussian blur can be created using this filter primitive, the special gaussian blur primitive is faster and resolution-independent."));
            break;
        case(NR::NR_FILTER_DIFFUSELIGHTING):
            _infobox_icon.set(g_strdup_printf("%s/feDiffuseLighting-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feDiffuseLighting</b> and feSpecularLighting filter primitives create \"embossed\" shadings.  The input's alpha channel is used to provide depth information: higher opacity areas are raised toward the viewer and lower opacity areas recede away from the viewer."));
            break;
        case(NR::NR_FILTER_DISPLACEMENTMAP):
            _infobox_icon.set(g_strdup_printf("%s/feDisplacementMap-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feDisplacementMap</b> filter primitive displaces the pixels in the first input using the second input as a displacement map, that shows from how far the pixel should come from. Classical examples are whirl and pinch effects."));
            break;
        case(NR::NR_FILTER_FLOOD):
            _infobox_icon.set(g_strdup_printf("%s/feFlood-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feFlood</b> filter primitive fills the region with a given color and opacity.  It is usually used as an input to other filters to apply color to a graphic."));
            break;
        case(NR::NR_FILTER_GAUSSIANBLUR):
            _infobox_icon.set(g_strdup_printf("%s/feGaussianBlur-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feGaussianBlur</b> filter primitive uniformly blurs its input.  It is commonly used together with feOffset to create a drop shadow effect."));
            break;
        case(NR::NR_FILTER_IMAGE):
            _infobox_icon.set(g_strdup_printf("%s/feImage-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feImage</b> filter primitive fills the region with an external image or another part of the document."));
            break;
        case(NR::NR_FILTER_MERGE):
            _infobox_icon.set(g_strdup_printf("%s/feMerge-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feMerge</b> filter primitive composites several temporary images inside the filter primitive to a single image. It uses normal alpha compositing for this. This is equivalent to using several feBlend primitives in 'normal' mode or several feComposite primitives in 'over' mode."));
            break;
        case(NR::NR_FILTER_MORPHOLOGY):
            _infobox_icon.set(g_strdup_printf("%s/feMorphology-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feMorphology</b> filter primitive provides erode and dilate effects. For single-colour objects erode makes the object thinner and dilate makes it thicker."));
            break;
        case(NR::NR_FILTER_OFFSET):
            _infobox_icon.set(g_strdup_printf("%s/feOffset-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feOffset</b> filter primitive offsets the image by an user-defined amount. For example, this is useful for drop shadows, where the shadow is in a slightly different position than the actual object."));
            break;
        case(NR::NR_FILTER_SPECULARLIGHTING):
            _infobox_icon.set(g_strdup_printf("%s/feSpecularLighting-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The feDiffuseLighting and <b>feSpecularLighting</b> filter primitives create \"embossed\" shadings.  The input's alpha channel is used to provide depth information: higher opacity areas are raised toward the viewer and lower opacity areas recede away from the viewer."));
            break;
        case(NR::NR_FILTER_TILE):
            _infobox_icon.set(g_strdup_printf("%s/feTile-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feTile</b> filter primitive tiles a region with its input graphic"));
            break;
        case(NR::NR_FILTER_TURBULENCE):
            _infobox_icon.set(g_strdup_printf("%s/feTurbulence-icon.png", INKSCAPE_PIXMAPDIR));
            _infobox_desc.set_markup(_("The <b>feTurbulence</b> filter primitive renders Perlin noise. This kind of noise is useful in simulating several nature phenomena like clouds, fire and smoke and in generating complex textures like marble or granite."));
            break;
    }
}

void FilterEffectsDialog::duplicate_primitive()
{
    SPFilter* filter = _filter_modifier.get_selected_filter();
    SPFilterPrimitive* origprim = _primitive_list.get_selected();

    if(filter && origprim) {
        Inkscape::XML::Node *repr;
        repr = SP_OBJECT_REPR(origprim)->duplicate(SP_OBJECT_REPR(origprim)->document());
        SP_OBJECT_REPR(filter)->appendChild(repr);

        sp_document_done(filter->document, SP_VERB_DIALOG_FILTER_EFFECTS, _("Duplicate filter primitive"));

        _primitive_list.update();
    }
}

void FilterEffectsDialog::convolve_order_changed()
{
    _convolve_matrix->set_from_attribute(SP_OBJECT(_primitive_list.get_selected()));
    _convolve_target->get_spinbuttons()[0]->get_adjustment()->set_upper(_convolve_order->get_spinbutton1().get_value() - 1);
    _convolve_target->get_spinbuttons()[1]->get_adjustment()->set_upper(_convolve_order->get_spinbutton2().get_value() - 1);
}

void FilterEffectsDialog::set_attr_direct(const AttrWidget* input)
{
    set_attr(_primitive_list.get_selected(), input->get_attribute(), input->get_as_attribute().c_str());
}

void FilterEffectsDialog::set_child_attr_direct(const AttrWidget* input)
{
    set_attr(_primitive_list.get_selected()->children, input->get_attribute(), input->get_as_attribute().c_str());
}

void FilterEffectsDialog::set_attr(SPObject* o, const SPAttributeEnum attr, const gchar* val)
{
    if(!_locked) {
        _attr_lock = true;

        SPFilter *filter = _filter_modifier.get_selected_filter();
        const gchar* name = (const gchar*)sp_attribute_name(attr);
        if(filter && name && o) {
            update_settings_sensitivity();

            SP_OBJECT_REPR(o)->setAttribute(name, val);
            filter->requestModified(SP_OBJECT_MODIFIED_FLAG);

            Glib::ustring undokey = "filtereffects:";
            undokey += name;
            sp_document_maybe_done(filter->document, undokey.c_str(), SP_VERB_DIALOG_FILTER_EFFECTS,
                                   _("Set filter primitive attribute"));
        }

        _attr_lock = false;
    }
}

void FilterEffectsDialog::update_settings_view()
{
    update_settings_sensitivity();

    if(_attr_lock)
        return;

    SPFilterPrimitive* prim = _primitive_list.get_selected();

    if(prim) {
        _settings->show_and_update(FPConverter.get_id_from_key(prim->repr->name()), prim);
        _empty_settings.hide();
    }
    else {
        _settings_box.hide_all();
        _settings_box.show();
        _empty_settings.show();
    }
}

void FilterEffectsDialog::update_settings_sensitivity()
{
    SPFilterPrimitive* prim = _primitive_list.get_selected();
    const bool use_k = SP_IS_FECOMPOSITE(prim) && SP_FECOMPOSITE(prim)->composite_operator == COMPOSITE_ARITHMETIC;
    _k1->set_sensitive(use_k);
    _k2->set_sensitive(use_k);
    _k3->set_sensitive(use_k);
    _k4->set_sensitive(use_k);

    if(SP_IS_FECOMPONENTTRANSFER(prim)) {
        SPFeComponentTransfer* ct = SP_FECOMPONENTTRANSFER(prim);
        const bool linear = ct->type == COMPONENTTRANSFER_TYPE_LINEAR;
        const bool gamma = ct->type == COMPONENTTRANSFER_TYPE_GAMMA;

        // Component transfer not yet implemented
        /*_ct_table->set_sensitive(ct->type == COMPONENTTRANSFER_TYPE_TABLE || ct->type == COMPONENTTRANSFER_TYPE_DISCRETE);
        _ct_slope->set_sensitive(linear);
        _ct_intercept->set_sensitive(linear);
        _ct_amplitude->set_sensitive(gamma);
        _ct_exponent->set_sensitive(gamma);
        _ct_offset->set_sensitive(gamma);*/
    }
}

void FilterEffectsDialog::update_color_matrix()
{
    _color_matrix_values->set_from_attribute(_primitive_list.get_selected());
}

} // namespace Dialog
} // namespace UI
} // namespace Inkscape

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=99 :
