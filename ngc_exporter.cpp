/*
 * This file is part of pcb2gcode.
 * 
 * Copyright (C) 2009, 2010 Patrick Birnzain <pbirnzain@users.sourceforge.net> and others
 * Copyright (C) 2010 Bernhard Kubicek <kubicek@gmx.at>
 * Copyright (C) 2013 Erik Schuster <erik@muenchen-ist-toll.de>
 * Copyright (C) 2014, 2015 Nicola Corna <nicola@corna.info>
 *
 * pcb2gcode is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * pcb2gcode is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with pcb2gcode.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ngc_exporter.hpp"
#include "options.hpp"
#include <boost/algorithm/string.hpp>
#include <iostream>
using std::cerr;
using std::flush;
using std::ios_base;
using std::left;
#include <string>
using std::to_string;
using std::string;
using std::cout;
using std::endl;

#include <vector>
using std::vector;

#include <cmath>
using std::ceil;

#include <memory>
using std::shared_ptr;
using std::dynamic_pointer_cast;

#include <iomanip>

#include <glibmm/miscutils.h>
using Glib::build_filename;

#include <boost/format.hpp>
using boost::format;

#include "units.hpp"

/******************************************************************************/
/*
 */
/******************************************************************************/
NGC_Exporter::NGC_Exporter(shared_ptr<Board> board)
    : Exporter(board), dpi(board->get_dpi()), 
      quantization_error( 2.0 / dpi ), ocodes(1), globalVars(100)
{
    this->board = board;
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void NGC_Exporter::add_header(string header)
{
    this->header.push_back(header);
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void NGC_Exporter::export_all(boost::program_options::variables_map& options)
{

    bMetricinput = options["metric"].as<bool>();      //set flag for metric input
    bMetricoutput = options["metricoutput"].as<bool>();      //set flag for metric output
    bZchangeG53 = options["zchange-absolute"].as<bool>();
    string outputdir = options["output-dir"].as<string>();
    
    //set imperial/metric conversion factor for output coordinates depending on metricoutput option
    cfactor = bMetricoutput ? 25.4 : 1;
    
    if( options["zero-start"].as<bool>() )
    {
        xoffset = board->get_min_x();
        yoffset = board->get_min_y();
    }
    else
    {
        xoffset = 0;
        yoffset = 0;
    }
    xoffset -= options["x-offset"].as<Length>().asInch(bMetricinput ? 1.0/25.4 : 1);
    yoffset -= options["y-offset"].as<Length>().asInch(bMetricinput ? 1.0/25.4 : 1);

    tileInfo = Tiling::generateTileInfo( options, ocodes, board->get_height(), board->get_width() );

    for ( string layername : board->list_layers() )
    {
        if (options["zero-start"].as<bool>()) {
            xoffset = board->get_min_x();
            yoffset = board->get_min_y();
        } else {
            xoffset = 0;
            yoffset = 0;
        }
        xoffset -= options["x-offset"].as<Length>().asInch(bMetricinput ? 1.0/25.4 : 1);
        yoffset -= options["y-offset"].as<Length>().asInch(bMetricinput ? 1.0/25.4 : 1);
        if (layername == "back" ||
            (layername == "outline" && !workSide(options, "cut"))) {
            xoffset = -xoffset + tileInfo.boardWidth*(tileInfo.tileX-1);
            xoffset -= 2 * options["mirror-axis"].as<Length>().asInch(bMetricinput ? 1.0/25.4 : 1);
        }

        boost::optional<autoleveller> leveller = boost::none;
        if ((options["al-front"].as<bool>() && layername == "front") ||
            (options["al-back"].as<bool>() && layername == "back")) {
          leveller.emplace(options, &ocodes, &globalVars, quantization_error,
                           xoffset, yoffset, tileInfo);
        }

        std::stringstream option_name;
        option_name << layername << "-output";
        string of_name = build_filename(outputdir, options[option_name.str()].as<string>());
        cout << "Exporting " << layername << "... " << flush;
        export_layer(board->get_layer(layername), of_name, leveller);
        cout << "DONE." << " (Height: " << board->get_height() * cfactor
             << (bMetricoutput ? "mm" : "in") << " Width: "
             << board->get_width() * cfactor << (bMetricoutput ? "mm" : "in")
             << ")";
        if (layername == "outline")
            cout << " The board should be cut from the " << ( workSide(options, "cut") ? "FRONT" : "BACK" ) << " side. ";
        cout << endl;
    }
}

/* Assume that we start at a safe height above the first point in path.  Cut
 * around the path, handling bridges where needed.  The bridges are identified
 * by where the bridges begins.  So the bridges is from points with indecies x
 * to x+1 for each element in the bridges vector.  We can always assume that the
 * bridge segment and the segments on either side form a straight line. */
void NGC_Exporter::cutter_milling(std::ofstream& of, shared_ptr<Cutter> cutter, shared_ptr<icoords> path,
                                  const vector<size_t>& bridges, const double xoffsetTot, const double yoffsetTot) {
  const unsigned int steps_num = ceil(-cutter->zwork / cutter->stepsize);

  for (unsigned int i = 0; i < steps_num; i++) {
    const double z = cutter->zwork / steps_num * (i + 1);

    of << "G01 Z" << z * cfactor << " F" << cutter->vertfeed * cfactor << " ( plunge. )\n";
    of << "G04 P0 ( dwell for no time -- G64 should not smooth over this point )\n";
    of << "G01 F" << cutter->feed * cfactor << "\n";

    auto current_bridge = bridges.cbegin();

    for (size_t current = 1; current < path->size(); current++) {
      if (current_bridge != bridges.cend() && *current_bridge == current && z >= cutter->bridges_height) {
        // We're about to cut to the start of a bridge but there is no need to
        // stop there so let's just mill right across it.
        current += 2;
        current_bridge++;
      }
      // We are now cutting to current.
      // Is this a bridge cut?
      auto is_bridge_cut = current_bridge != bridges.cend() && *current_bridge == current-1;
      if (is_bridge_cut) {
        // We're about to make a bridge cut so we need to go up.  (If we didn't
        // need to, we would have skipped over it earlier.)
        of << "G00 Z" << cutter->bridges_height * cfactor << '\n';
      }

      // Now cut horizontally.
      of << "G01 X" << (path->at(current).first - xoffsetTot) * cfactor
         << " Y"    << (path->at(current).second - yoffsetTot) * cfactor << '\n';

      // Now plunge back down if needed.
      if (is_bridge_cut) {
        of << "G01 Z" << z * cfactor << " F" << cutter->vertfeed * cfactor << '\n';
        of << "G01 F" << cutter->feed * cfactor << '\n';
        current_bridge++; // We're done with this bridge.
      }
    }
  }
}

void NGC_Exporter::isolation_milling(std::ofstream& of, shared_ptr<RoutingMill> mill, shared_ptr<icoords> path,
                                     boost::optional<autoleveller>& leveller, const double xoffsetTot, const double yoffsetTot) {
  of << "G01 F" << mill->vertfeed * cfactor << '\n';

  if (leveller) {
    leveller->setLastChainPoint(icoordpair((path->begin()->first - xoffsetTot) * cfactor,
                                           (path->begin()->second - yoffsetTot) * cfactor));
    of << leveller->g01Corrected(icoordpair((path->begin()->first - xoffsetTot) * cfactor,
                                            (path->begin()->second - yoffsetTot) * cfactor));
  } else {
    if (!mill->pre_milling_gcode.empty()) {
      of << "( begin pre-milling-gcode )\n";
      of << mill->pre_milling_gcode << "\n";
      of << "( end pre-milling-gcode )\n";
    }
    of << "G01 Z" << mill->zwork * cfactor << "\n";
  }

  of << "G04 P0 ( dwell for no time -- G64 should not smooth over this point )\n";
  of << "G01 F" << mill->feed * cfactor << '\n';

  icoords::iterator iter = path->begin();

  while (iter != path->end()) {
    if (leveller) {
      of << leveller->addChainPoint(icoordpair((iter->first - xoffsetTot) * cfactor,
                                               (iter->second - yoffsetTot) * cfactor));
    } else {
      of << "G01 X" << (iter->first - xoffsetTot) * cfactor << " Y"
         << (iter->second - yoffsetTot) * cfactor << '\n';
    }
    ++iter;
  }
  if (!mill->post_milling_gcode.empty()) {
    of << "( begin post-milling-gcode )\n";
    of << mill->post_milling_gcode << "\n";
    of << "( end post-milling-gcode )\n";
  }
}


void NGC_Exporter::export_layer(shared_ptr<Layer> layer, string of_name, boost::optional<autoleveller> leveller) {
    string layername = layer->get_name();
    shared_ptr<RoutingMill> mill = layer->get_manufacturer();
    vector<vector<shared_ptr<icoords>>> all_toolpaths = layer->get_toolpaths();

    if (all_toolpaths.size() < 1) {
      return; // Nothing to do.
    }

    globalVars.getUniqueCode();
    globalVars.getUniqueCode();

    // open output file
    std::ofstream of;
    of.open(of_name.c_str());

    // write header to .ngc file
    for ( string s : header )
    {
        of << "( " << s << " )\n";
    }

    if( leveller || ( tileInfo.enabled && tileInfo.software != Software::CUSTOM ) )
        of << "( Gcode for " << tileInfo.software << " )\n";
    else
        of << "( Software-independent Gcode )\n";

    of.setf(ios_base::fixed);      //write floating-point values in fixed-point notation
    of.precision(5);              //Set floating-point decimal precision

    of << "\n" << preamble;       //insert external preamble

    if (bMetricoutput) {
      of << "G94 ( Millimeters per minute feed rate. )\n"
         << "G21 ( Units == Millimeters. )\n\n";
    } else {
      of << "G94 ( Inches per minute feed rate. )\n"
         << "G20 ( Units == INCHES. )\n\n";
    }

    of << "G90 ( Absolute coordinates. )\n"
       << "G00 S" << left << mill->speed << " ( RPM spindle speed. )\n";

    if (mill->explicit_tolerance) {
      of << "G64 P" << mill->tolerance * cfactor << " ( set maximum deviation from commanded toolpath )\n";
    }

    of << "G01 F" << mill->feed * cfactor << " ( Feedrate. )\n\n";

    if (leveller) {
      if(!leveller->prepareWorkarea(all_toolpaths)) {
        options::maybe_throw(std::string("Required number of probe points (") + std::to_string(leveller->requiredProbePoints()) +
                             ") exceeds the maximum number (" + std::to_string(leveller->maxProbePoints()) + "). "
                             "Reduce either al-x or al-y.", ERR_INVALIDPARAMETER);
      }

      leveller->header(of);
    }

    shared_ptr<Cutter> cutter = dynamic_pointer_cast<Cutter>(mill);
    shared_ptr<Isolator> isolator = dynamic_pointer_cast<Isolator>(mill);

    // One list of bridges for each path.
    vector<vector<size_t>> all_bridges;
    if (cutter) {
      for (const auto& path : all_toolpaths[0]) {  // Cutter layer can only have one tool_diameter.
        auto bridges = layer->get_bridges(path);
        all_bridges.push_back(bridges);
      }
    }

    uniqueCodes main_sub_ocodes(200);
    for (size_t toolpaths_index = 0; toolpaths_index < all_toolpaths.size(); toolpaths_index++) {
      const auto& toolpaths = all_toolpaths[toolpaths_index];
      if (toolpaths.size() < 1) {
        continue; // Nothing to do for this mill size.
      }
      Tiling tiling(tileInfo, cfactor, main_sub_ocodes.getUniqueCode());
      tiling.setGCodeEnd(string("\nG04 P0 ( dwell for no time -- G64 should not smooth over this point )\n")
                         + (bZchangeG53 ? "G53 " : "") + "G00 Z" + str( format("%.3f") % ( mill->zchange * cfactor ) ) +
                         " ( retract )\n\n" + postamble + "M5 ( Spindle off. )\nG04 P" +
                         to_string(mill->spindown_time) + "\n");

      // Start the new tool.
      of << endl
         << "G00 Z" << mill->zchange * cfactor << " (Retract to tool change height)" << endl
         << "T" << toolpaths_index << endl
         << "M5      (Spindle stop.)" << endl
         << "G04 P" << mill->spindown_time << " (Wait for spindle to stop)" << endl;
      if (cutter) {
        of << "(MSG, Change tool bit to cutter diameter ";
        if (bMetricoutput) {
          of << (cutter->tool_diameter * 25.4) << "mm)" << endl;
        } else {
          of << cutter->tool_diameter << "in)" << endl;
        }
      } else if (isolator) {
        of << "(MSG, Change tool bit to mill diameter ";
        if (bMetricoutput) {
          of << (isolator->tool_diameters_and_overlap_widths[toolpaths_index].first * 25.4) << "mm)" << endl;
        } else {
          of << isolator->tool_diameters_and_overlap_widths[toolpaths_index].first << "in)" << endl;
        }
      } else {
        throw std::logic_error("Can't cast to Cutter nor Isolator.");
      }
      of << "M6      (Tool change.)" << endl
         << "M0      (Temporary machine stop.)" << endl
         << "M3 ( Spindle on clockwise. )" << endl
         << "G04 P" << mill->spinup_time << " (Wait for spindle to get up to speed)" << endl;

      tiling.header( of );

      for( unsigned int i = 0; i < tileInfo.forYNum; i++ ) {
        double yoffsetTot = yoffset - i * tileInfo.boardHeight;
        for( unsigned int j = 0; j < tileInfo.forXNum; j++ ) {
          double xoffsetTot = xoffset - ( i % 2 ? tileInfo.forXNum - j - 1 : j ) * tileInfo.boardWidth;

          if( tileInfo.enabled && tileInfo.software == Software::CUSTOM )
            of << "( Piece #" << j + 1 + i * tileInfo.forXNum << ", position [" << j << ";" << i << "] )\n\n";

          // contours
          for(size_t path_index = 0; path_index < toolpaths.size(); path_index++) {
            shared_ptr<icoords> path = toolpaths[path_index];
            if (path->size() < 1) {
              continue; // Empty path.
            }

            // retract, move to the starting point of the next contour
            of << "G04 P0 ( dwell for no time -- G64 should not smooth over this point )\n";
            of << "G00 Z" << mill->zsafe * cfactor << " ( retract )" << endl << endl;
            of << "G00 X" << ( path->begin()->first - xoffsetTot ) * cfactor << " Y"
               << ( path->begin()->second - yoffsetTot ) * cfactor << " ( rapid move to begin. )\n";

            /* if we're cutting, perhaps do it in multiple steps, but do isolations just once.
             * i know this is partially repetitive, but this way it's easier to read
             */
            if (cutter) {
              cutter_milling(of, cutter, path, all_bridges[path_index], xoffsetTot, yoffsetTot);
            } else {
              isolation_milling(of, mill, path, leveller, xoffsetTot, yoffsetTot);
            }
          }
        }
      }

      tiling.footer( of );
    }
    if (leveller) {
      leveller->footer(of);
    }
    of << "M9 ( Coolant off. )" << endl
       << "M2 ( Program end. )" << endl << endl;


    of.close();
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void NGC_Exporter::set_preamble(string _preamble)
{
    preamble = _preamble;
}

/******************************************************************************/
/*
 */
/******************************************************************************/
void NGC_Exporter::set_postamble(string _postamble)
{
    postamble = _postamble;
}
