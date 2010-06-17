/*
Copyright (c) 2009, Regents of the University of Alaska

All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
    * Neither the name of the Geographic Information Network of Alaska nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

This code was developed by Dan Stahlke for the Geographic Information Network of Alaska.
*/



#include "common.h"
#include "polygon.h"
#include "polygon-rasterizer.h"
#include "debugplot.h"
#include "georef.h"
#include "ndv.h"
#include "mask.h"
#include "mask-tracer.h"
#include "dp.h"
#include "excursion_pincher.h"
#include "beveler.h"

#include <ogrsf_frmts.h>
#include <cpl_string.h>
#include <cpl_conv.h>
#include <cpl_port.h>

#ifdef CPL_MSB 
#define WKB_BYTE_ORDER wkbNDR
#else
#define WKB_BYTE_ORDER wkbXDR
#endif

#define CS_UNKNOWN 0
#define CS_XY 1
#define CS_EN 2
#define CS_LL 3

using namespace dangdal;

// FIXME - describe setting out-cs and ogr-fmt only before output is specified
void usage(const char *cmdname) {
	printf("Usage:\n  %s [options] [image_name]\n", cmdname);
	printf("\n");
	
	print_georef_usage();
	printf("\n");
	print_ndv_usage();

	printf("\
\n\
Behavior:\n\
  -classify                    Output a polygon for each value of an 8-bit band\n\
                               (default is to generate a single polygon that\n\
                               surrounds all pixels that don't match\n\
                               the no-data-value)\n\
  -b band_id -b band_id ...    Bands to inspect (default is all bands)\n\
  -invert                      Trace no-data pixels rather than data pixels\n\
  -erosion                     Erode pixels that don't have two consecutive\n\
                               neighbors\n\
  -major-ring                  Take only the biggest outer ring\n\
  -no-donuts                   Take only top-level rings\n\
  -min-ring-area val           Drop rings with less than this area\n\
                               (in square pixels)\n\
  -dp-toler val                Tolerance for point reduction\n\
                               (in pixels, default is 2.0)\n\
  -bevel-size                  How much to shave off corners at\n\
                               self-intersection points\n\
                               (in pixels, default it 0.1)\n\
                               (this is done to make geometries that\n\
                               PostGIS/GEOS/Jump can handle)\n\
  -pinch-excursions            Remove all the complicated 'mouse bites' that\n\
                               occur in the outline when lossy compression\n\
                               has been used (experimental)\n\
\n\
Output:\n\
  -report fn.ppm               Output graphical report of polygons found\n\
  -mask-out fn.pbm             Output mask of bounding polygon in PBM format\n\
  -out-cs [xy | en | ll]       Set coordinate system for following outputs\n\
                               (pixel coords, easting/northing, or lon/lat)\n\
  -llproj-toler val            Error tolerance for curved lines when\n\
                               using '-out-cs ll' (in pixels, default is 1.0)\n\
  -wkt-out fn.wkt              Output polygons in WKT format\n\
  -wkb-out fn.wkb              Output polygons in WKB format\n\
  -ogr-out fn.shp              Output polygons using an OGR format\n\
  -ogr-fmt                     OGR format to use (default is 'ESRI Shapefile')\n\
  -split-polys                 Output several polygons rather than one\n\
                               multipolygon\n\
\n\
Misc:\n\
  -v                           Verbose\n\
\n\
Examples:\n\
\n\
Inspect image and output contour of data region:\n\
gdal_trace_outline raster.tif -nodataval 0 -erosion -out-cs ll -wkt-out outline.wkt\n\
\n\
Same as above but polygon actually follows border pixel-by-pixel:\n\
gdal_trace_outline raster.tif -nodataval 0 -dp-toler 0 -out-cs ll -wkt-out outline.wkt\n\
\n\
Output ESRI Shapefile in projection of input image:\n\
gdal_trace_outline raster.tif -nodataval 0 -erosion -out-cs en -ogr-out outline.shp\n\
\n\
Generate one shape for each value in input image:\n\
gdal_trace_outline raster.tif -classify -out-cs en -ogr-out outline.shp\n\
\n\
");
	exit(1);
}

Mpoly calc_ring_from_mask(BitGrid mask, size_t w, size_t h,
	bool major_ring_only, bool no_donuts,
	long min_ring_area, double bevel_size);

typedef struct {
	int out_cs;

	const char *wkt_fn;
	FILE *wkt_fh;

	const char *wkb_fn;
	FILE *wkb_fh;

	const char *ogr_fn;
	const char *ogr_fmt;
	OGRDataSourceH ogr_ds;
	OGRLayerH ogr_layer;
	int class_fld_idx;
	int color_fld_idx[4];
} geom_output_t;

typedef struct {
	geom_output_t *output;
	int num;
} geom_output_list_t;

geom_output_t *add_geom_output(geom_output_list_t *list, int out_cs) {
	if(out_cs == CS_UNKNOWN) fatal_error(
		"must specify output coordinate system with -out-cs option before specifying output");
	list->output = REMYALLOC(geom_output_t, list->output, (list->num + 1));
	geom_output_t *go = list->output + list->num;
	list->num++;

	go->out_cs = out_cs;

	go->wkt_fn = NULL;
	go->wkb_fn = NULL;
	go->ogr_fn = NULL;

	go->wkt_fh = NULL;
	go->wkb_fh = NULL;
	go->ogr_ds = NULL;

	return go;
}

int main(int argc, char **argv) {
	const char *input_raster_fn = NULL;
	bool classify = 0;
	const char *debug_report = NULL;
	int inspect_numbands = 0;
	int *inspect_bandids = NULL;
	bool split_polys = 0;
	int cur_out_cs = CS_UNKNOWN;
	const char *cur_ogr_fmt = "ESRI Shapefile";
	geom_output_list_t geom_outputs = (geom_output_list_t){NULL, 0};
	const char *mask_out_fn = NULL;
	bool major_ring_only = 0;
	bool no_donuts = 0;
	long min_ring_area = 0;
	double reduction_tolerance = 2;
	bool do_erosion = 0;
	bool do_invert = 0;
	double llproj_toler = 1;
	double bevel_size = .1;
	bool do_pinch_excursions = 0;

	if(argc == 1) usage(argv[0]);

	geo_opts_t geo_opts = init_geo_options(&argc, &argv);
	ndv_def_t ndv_def = init_ndv_options(&argc, &argv);

	int argp = 1;
	while(argp < argc) {
		const char *arg = argv[argp++];
		// FIXME - check duplicate values
		if(arg[0] == '-') {
			if(!strcmp(arg, "-v")) {
				VERBOSE++;
			} else if(!strcmp(arg, "-classify")) {
				classify = 1;
			} else if(!strcmp(arg, "-report")) {
				if(argp == argc) usage(argv[0]);
				debug_report = argv[argp++];
			} else if(!strcmp(arg, "-b")) {
				if(argp == argc) usage(argv[0]);
				char *endptr;
				int bandid = strtol(argv[argp++], &endptr, 10);
				if(*endptr) usage(argv[0]);
				inspect_bandids = REMYALLOC(int, inspect_bandids, (inspect_numbands+1));
				inspect_bandids[inspect_numbands++] = bandid;
			} else if(!strcmp(arg, "-erosion")) {
				do_erosion = 1;
			} else if(!strcmp(arg, "-invert")) {
				do_invert = 1;
			} else if(!strcmp(arg, "-split-polys")) {
				split_polys = 1;
			} else if(!strcmp(arg, "-wkt-out")) {
				if(argp == argc) usage(argv[0]);
				geom_output_t *go = add_geom_output(&geom_outputs, cur_out_cs);
				go->wkt_fn = argv[argp++];
			} else if(!strcmp(arg, "-wkb-out")) {
				if(argp == argc) usage(argv[0]);
				geom_output_t *go = add_geom_output(&geom_outputs, cur_out_cs);
				go->wkb_fn = argv[argp++];
			} else if(!strcmp(arg, "-ogr-out")) {
				if(argp == argc) usage(argv[0]);
				geom_output_t *go = add_geom_output(&geom_outputs, cur_out_cs);
				go->ogr_fmt = cur_ogr_fmt;
				go->ogr_fn = argv[argp++];
			} else if(!strcmp(arg, "-ogr-fmt")) {
				if(argp == argc) usage(argv[0]);
				cur_ogr_fmt = argv[argp++];
			} else if(!strcmp(arg, "-out-cs")) {
				if(argp == argc) usage(argv[0]);
				const char *cs = argv[argp++];
				if(!strcmp(cs, "xy")) cur_out_cs = CS_XY;
				else if(!strcmp(cs, "en")) cur_out_cs = CS_EN;
				else if(!strcmp(cs, "ll")) cur_out_cs = CS_LL;
				else fatal_error("unrecognized value for -out-cs option (%s)", cs);
			} else if(!strcmp(arg, "-mask-out")) {
				if(argp == argc) usage(argv[0]);
				mask_out_fn = argv[argp++];
			} else if(!strcmp(arg, "-major-ring")) {
				major_ring_only = 1;
			} else if(!strcmp(arg, "-no-donuts")) {
				no_donuts = 1;
			} else if(!strcmp(arg, "-min-ring-area")) {
				if(argp == argc) usage(argv[0]);
				char *endptr;
				min_ring_area = strtol(argv[argp++], &endptr, 10);
				if(*endptr) usage(argv[0]);
			} else if(!strcmp(arg, "-dp-toler")) {
				if(argp == argc) usage(argv[0]);
				char *endptr;
				reduction_tolerance = strtod(argv[argp++], &endptr);
				if(*endptr) usage(argv[0]);
			} else if(!strcmp(arg, "-bevel-size")) {
				if(argp == argc) usage(argv[0]);
				char *endptr;
				bevel_size = strtod(argv[argp++], &endptr);
				if(*endptr) usage(argv[0]);
				if(bevel_size < 0 || bevel_size >= 1) fatal_error(
					"-bevel-size must be in the range 0 <= bevel < 1");
			} else if(!strcmp(arg, "-pinch-excursions")) { // FIXME - document
				do_pinch_excursions = 1;
			} else if(!strcmp(arg, "-llproj-toler")) {
				if(argp == argc) usage(argv[0]);
				char *endptr;
				llproj_toler = strtod(argv[argp++], &endptr);
				if(*endptr) usage(argv[0]);
			} else if(!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
				usage(argv[0]);
			} else {
				fatal_error("unrecognized option: %s", arg);
			}
		} else {
			if(input_raster_fn) usage(argv[0]);
			input_raster_fn = arg;
		}
	}

	if(!input_raster_fn) fatal_error("must specify filename of image");

	bool do_geom_output = geom_outputs.num;

	if(major_ring_only && min_ring_area) fatal_error(
		"-major-ring and -min-ring-area options cannot both be used at the same time");
	if(major_ring_only && no_donuts) fatal_error(
		"-major-ring and -no-donuts options cannot both be used at the same time");

	if(classify) {
		if(ndv_def.nranges) fatal_error("-classify option is not compatible with NDV options");
		if(do_invert) fatal_error("-classify option is not compatible with -invert option");
		if(mask_out_fn) fatal_error("-classify option is not compatible with -mask-out option");
	}

	GDALAllRegister();

	GDALDatasetH ds = GDALOpen(input_raster_fn, GA_ReadOnly);
	if(!ds) fatal_error("open failed");

	if(!inspect_numbands) {
		inspect_numbands = classify ? 1 : GDALGetRasterCount(ds);
		inspect_bandids = MYALLOC(int, inspect_numbands);
		for(int i=0; i<inspect_numbands; i++) inspect_bandids[i] = i+1;
	}

	// FIXME - optional NDV for classify
	if(!classify) {
		if(!ndv_def.nranges) {
			add_ndv_from_raster(&ndv_def, ds, inspect_numbands, inspect_bandids);
		}
	}

	CPLPushErrorHandler(CPLQuietErrorHandler);

	georef_t georef = init_georef(&geo_opts, ds);

	for(int i=0; i<geom_outputs.num; i++) {
		int out_cs = geom_outputs.output[i].out_cs;
		if((out_cs == CS_EN || out_cs == CS_LL) && !georef.fwd_affine) 
			fatal_error("missing affine transform");
		if((out_cs == CS_LL) && !georef.fwd_xform) 
			fatal_error("missing coordinate transform");
	}

	report_image_t *dbuf = NULL;
	if(debug_report) {
		dbuf = create_plot(georef.w, georef.h);
		dbuf->mode = do_pinch_excursions ? PLOT_PINCH : PLOT_CONTOURS;
	}

	const uint8_t *raster = NULL;
	BitGrid mask(0, 0);
	uint8_t usage_array[256];
	GDALColorTableH color_table = NULL;
	if(classify) {
		if(inspect_numbands != 1) {
			fatal_error("only one band may be used in classify mode");
		}

		raster = read_dataset_8bit(ds, inspect_bandids[0], usage_array, dbuf);

		GDALRasterBandH band = GDALGetRasterBand(ds, inspect_bandids[0]);
		if(GDALGetRasterColorInterpretation(band) == GCI_PaletteIndex) {
			color_table = GDALGetRasterColorTable(band);
		}
	} else {
		mask = get_bitgrid_for_dataset(
			ds, inspect_numbands, inspect_bandids,
			&ndv_def, dbuf);
	}

	for(int go_idx=0; go_idx<geom_outputs.num; go_idx++) {
		geom_output_t *go = geom_outputs.output + go_idx;
		
		if(go->wkt_fn) {
			go->wkt_fh = fopen(go->wkt_fn, "w");
			if(!go->wkt_fh) fatal_error("cannot open output file for WKT");
		}
		if(go->wkb_fn) {
			go->wkb_fh = fopen(go->wkb_fn, "w");
			if(!go->wkb_fh) fatal_error("cannot open output file for WKB");
		}

		if(go->ogr_fn) {
			OGRRegisterAll();
			if(!go->ogr_fmt) fatal_error("no OGR format was specified");
			OGRSFDriverH ogr_driver = OGRGetDriverByName(go->ogr_fmt);
			if(!ogr_driver) fatal_error("cannot get OGR driver (%s)", go->ogr_fmt);
			go->ogr_ds = OGR_Dr_CreateDataSource(ogr_driver, go->ogr_fn, NULL);
			if(!go->ogr_ds) fatal_error("cannot create OGR data source");

			const char *layer_name = go->ogr_fn;

			OGRSpatialReferenceH sref = NULL;
			if(go->out_cs == CS_EN) {
				sref = georef.spatial_ref;
			} else if(go->out_cs == CS_LL) {
				sref = georef.geo_sref;
			}

			go->ogr_layer = OGR_DS_CreateLayer(go->ogr_ds, layer_name, sref, 
				(split_polys ? wkbPolygon : wkbMultiPolygon), NULL);
			if(!go->ogr_layer) fatal_error("cannot create OGR layer");

			go->class_fld_idx = -1;
			for(int i=0; i<4; i++) go->color_fld_idx[i] = -1;
			if(classify) {
				OGRFieldDefnH fld = OGR_Fld_Create("value", OFTInteger);
				OGR_Fld_SetWidth(fld, 4);
				OGR_L_CreateField(go->ogr_layer, fld, TRUE);
				go->class_fld_idx = OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(go->ogr_layer), "value");

				if(color_table) {
					const char *names[4] = { "c1", "c2", "c3", "c4" };
					for(int i=0; i<4; i++) {
						fld = OGR_Fld_Create(names[i], OFTInteger);
						OGR_Fld_SetWidth(fld, 4);
						OGR_L_CreateField(go->ogr_layer, fld, TRUE);
						go->color_fld_idx[i] = OGR_FD_GetFieldIndex(OGR_L_GetLayerDefn(go->ogr_layer), names[i]);
					}
				}
			}
		}
	}

	int num_shapes_written = 0;

	for(int class_id=0; class_id<256; class_id++) {
		const GDALColorEntry *color = NULL;
		if(classify) {
			if(!usage_array[class_id]) continue;
			printf("\nFeature class %d\n", class_id);

			if(color_table) {
				color = GDALGetColorEntry(color_table, class_id);
				if(color) printf("  Color=%d,%d,%d,%d\n",
					color->c1, color->c2, color->c3, color->c4);
			}

			mask = get_bitgrid_for_8bit_raster(georef.w, georef.h,
				raster, (uint8_t)class_id);
		} else {
			if(class_id != 0) continue;
		}

		if(do_invert) {
			mask.invert();
		}

		if(do_erosion) {
			mask.erode();
		}

		Mpoly feature_poly = calc_ring_from_mask(mask, georef.w, georef.h,
			major_ring_only, no_donuts, min_ring_area, bevel_size);
		mask = BitGrid(0, 0); // free some memory

		if(feature_poly.rings.size() && do_pinch_excursions) {
			printf("Pinching excursions...\n");
			feature_poly = pinch_excursions2(feature_poly, dbuf);
			printf("Done pinching excursions.\n");
		}

		if(mask_out_fn) {
			mask_from_mpoly(feature_poly, georef.w, georef.h, mask_out_fn);
		}

		if(feature_poly.rings.size() && reduction_tolerance > 0) {
			Mpoly reduced_poly = compute_reduced_pointset(feature_poly, reduction_tolerance);
			feature_poly = reduced_poly;
		}

		if(feature_poly.rings.size()) {
			size_t num_outer=0, num_inner=0, total_pts=0;
			for(size_t r_idx=0; r_idx<feature_poly.rings.size(); r_idx++) {
				if(feature_poly.rings[r_idx].is_hole) num_inner++;
				else num_outer++;
				total_pts += feature_poly.rings[r_idx].pts.size();
			}
			printf("Found %zd outer rings and %zd holes with a total of %zd vertices.\n",
				num_outer, num_inner, total_pts);

			if(dbuf && dbuf->mode == PLOT_CONTOURS) {
				debug_plot_mpoly(dbuf, feature_poly);
			}

			if(do_geom_output && feature_poly.rings.size()) {
				printf("Writing output\n");

				std::vector<Mpoly> shapes;
				if(split_polys) {
					shapes = split_mpoly_to_polys(feature_poly);
				} else {
					shapes.push_back(feature_poly);
				}

				for(size_t shape_idx=0; shape_idx<shapes.size(); shape_idx++) {
					const Mpoly &poly_in = shapes[shape_idx];

					for(int go_idx=0; go_idx<geom_outputs.num; go_idx++) {
						geom_output_t *go = geom_outputs.output + go_idx;

						Mpoly proj_poly = poly_in;
						if(go->out_cs == CS_XY) {
							// no-op
						} else if(go->out_cs == CS_EN) {
							proj_poly.xy2en(&georef);
						} else if(go->out_cs == CS_LL) {
							proj_poly.xy2ll_with_interp(&georef, llproj_toler);
						} else {
							fatal_error("bad val for out_cs");
						}

						OGRGeometryH ogr_geom = mpoly_to_ogr(proj_poly);

						if(go->wkt_fh) {
							char *wkt_out;
							OGR_G_ExportToWkt(ogr_geom, &wkt_out);
							fprintf(go->wkt_fh, "%s\n", wkt_out);
						}
						if(go->wkb_fh) {
							size_t wkb_size = OGR_G_WkbSize(ogr_geom);
							printf("WKB size = %zd\n", wkb_size);
							unsigned char *wkb_out = MYALLOC(unsigned char, wkb_size);
							OGR_G_ExportToWkb(ogr_geom, WKB_BYTE_ORDER, wkb_out);
							fwrite(wkb_out, wkb_size, 1, go->wkb_fh);
							free(wkb_out);
						}

						if(go->ogr_ds) {
							OGRFeatureH ogr_feat = OGR_F_Create(OGR_L_GetLayerDefn(go->ogr_layer));
							if(go->class_fld_idx >= 0) OGR_F_SetFieldInteger(ogr_feat, go->class_fld_idx, class_id);
							if(color) {
								if(go->color_fld_idx[0] >= 0) OGR_F_SetFieldInteger(
									ogr_feat, go->color_fld_idx[0], color->c1);
								if(go->color_fld_idx[1] >= 0) OGR_F_SetFieldInteger(
									ogr_feat, go->color_fld_idx[1], color->c2);
								if(go->color_fld_idx[2] >= 0) OGR_F_SetFieldInteger(
									ogr_feat, go->color_fld_idx[2], color->c3);
								if(go->color_fld_idx[3] >= 0) OGR_F_SetFieldInteger(
									ogr_feat, go->color_fld_idx[3], color->c4);
							}
							OGR_F_SetGeometryDirectly(ogr_feat, ogr_geom); // assumes ownership of geom
							OGR_L_CreateFeature(go->ogr_layer, ogr_feat);
							OGR_F_Destroy(ogr_feat);
						} else {
							OGR_G_DestroyGeometry(ogr_geom);
						}
					}

					num_shapes_written++;
				}
			}
		}
	}

	printf("\n");

	for(int go_idx=0; go_idx<geom_outputs.num; go_idx++) {
		geom_output_t *go = geom_outputs.output + go_idx;
		if(go->wkt_fh) fclose(go->wkt_fh);
		if(go->wkb_fh) fclose(go->wkb_fh);
		if(go->ogr_ds) OGR_DS_Destroy(go->ogr_ds);
	}

	if(dbuf) write_plot(dbuf, debug_report);

	if(do_geom_output) {
		if(num_shapes_written) printf("Wrote %d shapes.\n", num_shapes_written);
		else printf("Wrote empty shapefile.\n");
	}

	CPLPopErrorHandler();

	return 0;
}

Mpoly calc_ring_from_mask(BitGrid mask, size_t w, size_t h,
bool major_ring_only, bool no_donuts, 
long min_ring_area, double bevel_size) {
	if(major_ring_only) no_donuts = 1;

	Mpoly mp = trace_mask(mask, w, h, min_ring_area, no_donuts);

	if(VERBOSE) {
		size_t num_inner = 0, num_outer = 0, total_pts = 0;
		for(size_t r_idx=0; r_idx<mp.rings.size(); r_idx++) {
			if(mp.rings[r_idx].is_hole) num_inner++;
			else num_outer++;
			total_pts += mp.rings[r_idx].pts.size();
		}
		printf("tracer produced %zd rings (%zd outer, %zd holes) with a total of %zd points\n",
			mp.rings.size(), num_outer, num_inner, total_pts);
	}

	// this is now done directly by tracer
	/*
	if(min_ring_area > 0) {
		if(VERBOSE) printf("removing small rings...\n");

		ring_t *filtered_rings = MYALLOC(ring_t, mp.rings.size());
		int *parent_map = MYALLOC(int, mp.rings.size());
		for(int i=0; i<mp.rings.size(); i++) {
			parent_map[i] = -1;
		}
		int num_filtered_rings = 0;
		for(int i=0; i<mp.rings.size(); i++) {
			double area = ring_area(mp.rings+i);
			if(VERBOSE) if(area > 10) printf("ring %zd has area %.15f\n", i, area);
			if(area >= min_ring_area) {
				parent_map[i] = num_filtered_rings;
				filtered_rings[num_filtered_rings++] = mp.rings[i];
			} else {
				free_ring(mp.rings + i);
			}
		}
		for(int i=0; i<num_filtered_rings; i++) {
			int old_parent = filtered_rings[i].parent_id;
			if(old_parent >= 0) {
				int new_parent = parent_map[old_parent];
				if(new_parent < 0) fatal_error("took a ring but not its parent");
				filtered_rings[i].parent_id = new_parent;
			}
		}
		printf("filtered by area %zd => %zd rings\n",
			mp.rings.size(), num_filtered_rings);

		free(mp.rings);
		mp.rings = filtered_rings;
		mp.rings.size() = num_filtered_rings;
	}
	*/

	if(major_ring_only && mp.rings.size() > 1) {
		double biggest_area = 0;
		size_t best_idx = 0;
		for(size_t i=0; i<mp.rings.size(); i++) {
			double area = mp.rings[i].area();
			if(area > biggest_area) {
				biggest_area = area;
				best_idx = i;
			}
		}
		if(VERBOSE) printf("major ring was %zd with %zd pts, %.1f area\n",
			best_idx, mp.rings[best_idx].pts.size(), biggest_area);
		if(mp.rings[best_idx].parent_id >= 0) fatal_error("largest ring should not have a parent");

		Mpoly new_mp;
		new_mp.rings.push_back(mp.rings[best_idx]);
		mp = new_mp;
	}

	// this is now done directly by tracer
	/*
	if(no_donuts) {
		// we don't have to worry about remapping parent_id in this
		// case because we only take rings with no parent
		int out_idx = 0;
		for(int i=0; i<mp.rings.size(); i++) {
			if(mp.rings[i].parent_id < 0) {
				mp.rings[out_idx++] = mp.rings[i];
			}
		}
		mp.num_rings = out_idx;
		if(VERBOSE) printf("number of non-donut rings is %zd", mp.rings.size());
	}
	*/

	if(mp.rings.size() && bevel_size > 0) {
		// the topology cannot be resolved by us or by geos/jump/postgis if
		// there are self-intersections
		bevel_self_intersections(mp, bevel_size);
	}

	return mp;
}
