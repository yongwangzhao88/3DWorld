// 3D World - Voxel Code
// by Frank Gennari
// 2/25/12

#include "voxels.h"
#include "marching_cubes.h"
#include "upsurface.h" // for noise_gen_3d
#include "mesh.h"
#include "gl_ext_arb.h"
#include "shaders.h"
#include "file_utils.h"


bool const NORMALIZE_TO_1  = 1;
bool const DEBUG_BLOCKS    = 0;
unsigned const NUM_BLOCKS  = 8; // in x and y
unsigned const NOISE_TSIZE = 64;


voxel_params_t global_voxel_params;
voxel_model terrain_voxel_model;

extern bool disable_shaders, group_back_face_cull, scene_dlist_invalid;
extern int dynamic_mesh_scroll, rand_gen_index, scrolling;
extern coll_obj_group coll_objects;


template class voxel_grid<float>; // explicit instantiation


// if size==old_size, we do nothing;  if size==0, we only free the texture
void noise_texture_manager_t::setup(unsigned size, int rseed, float mag, float freq, vector3d const &offset) {

	if (size == tsize) return; // nothing to do
	clear();
	tsize = size;
	if (size == 0) return; // nothing else to do
	voxels.clear();
	voxels.init(tsize, tsize, tsize, vector3d(1,1,1), all_zeros, 0.0);
	voxels.create_procedural(mag, freq, offset, 1, rseed, 654+rand_gen_index);
	vector<unsigned char> data;
	data.resize(voxels.size());

	for (unsigned i = 0; i < voxels.size(); ++i) {
		data[i] = (unsigned char)(255*CLIP_TO_01(fabs(voxels[i]))); // use fabs() to convert from [-1,1] to [0,1]
	}
	noise_tid = create_3d_texture(tsize, tsize, tsize, 1, data, GL_LINEAR, GL_MIRRORED_REPEAT);
}


void noise_texture_manager_t::bind_texture(unsigned tu_id) const {

	assert(glIsTexture(noise_tid));
	set_multitex(tu_id);
	bind_3d_texture(noise_tid);
	set_multitex(0);
}


void noise_texture_manager_t::clear() {

	free_texture(noise_tid);
	tsize = 0;
}


float noise_texture_manager_t::eval_at(point const &pos) const {

	unsigned ix;
	if (!voxels.get_ix(pos, ix)) return 0.0; // off the voxel grid
	return voxels[ix];
}


template<typename V> void voxel_grid<V>::init(unsigned nx_, unsigned ny_, unsigned nz_, vector3d const &vsz_, point const &center_, V default_val) {

	vsz = vsz_;
	assert(vsz.x > 0.0 && vsz.y > 0.0 && vsz.z > 0.0);
	nx = nx_; ny = ny_; nz = nz_;
	xblocks = 1+(nx-1)/NUM_BLOCKS; // ceil
	yblocks = 1+(ny-1)/NUM_BLOCKS; // ceil
	unsigned const tot_size(nx * ny * nz);
	assert(tot_size > 0);
	clear();
	resize(tot_size, default_val);
	center = center_;
	lo_pos = center - 0.5*point(nx*vsz.x, ny*vsz.y, nz*vsz.z);
}


void voxel_manager::clear() {
	
	outside.clear();
	first_zval_above_mesh.clear();
	float_voxel_grid::clear();
}


void voxel_manager::create_procedural(float mag, float freq, vector3d const &offset, bool normalize_to_1, int rseed1, int rseed2) {

	noise_gen_3d ngen;
	ngen.set_rand_seeds(rseed1, rseed2);
	ngen.gen_sines(mag, freq); // create sine table
	unsigned const xyz_num[3] = {nx, ny, nz};
	vector<float> xyz_vals[3];
	ngen.gen_xyz_vals((lo_pos + offset), vsz, xyz_num, xyz_vals); // create xyz values

	#pragma omp parallel for schedule(static,1)
	for (int y = 0; y < (int)ny; ++y) { // generate voxel values
		for (unsigned x = 0; x < nx; ++x) {
			for (unsigned z = 0; z < nz; ++z) {
#if 1
				float val(ngen.get_val(x, y, z, xyz_vals));
#else
				point pos(get_pt_at(x, y, z));
				pos += 20.0*fabs(ngen.get_val(0.01*pos))*vector3d(1,1,1); // warp
				float val(ngen.get_val(pos));
#endif
				if (normalize_to_1) val = max(-1.0f, min(1.0f, val));
				set(x, y, z, val); // scale value?
			}
		}
	}
}


void voxel_manager::atten_edge_val(unsigned x, unsigned y, unsigned z, float val) {

	float const vy(1.0 - 2.0*max(0.0, (y - 0.5*ny))/float(ny));
	float const vx(1.0 - 2.0*fabs(x - 0.5*nx)/float(nx));
	float const vz(1.0 - 2.0*fabs(z - 0.5*nz)/float(nz)), v(0.25 - vx*vy*vz);
	if (v > 0.0) get_ref(x, y, z) += 8.0*val*v;
}


void voxel_manager::atten_at_edges(float val) { // and top (5 edges)

	for (unsigned y = 0; y < ny; ++y) {
		for (unsigned x = 0; x < nx; ++x) {
			for (unsigned z = 0; z < nz; ++z) {
				atten_edge_val(x, y, z, val);
			}
		}
	}
}


void voxel_manager::atten_top_val(unsigned x, unsigned y, unsigned z, float val) {

	float const zval(z/float(nz) - 0.75);
	if (zval > 0.0) get_ref(x, y, z) += 12.0*val*zval;
}


void voxel_manager::atten_at_top_only(float val) {

	for (unsigned y = 0; y < ny; ++y) {
		for (unsigned x = 0; x < nx; ++x) {
			for (unsigned z = 0; z < nz; ++z) {
				atten_top_val(x, y, z, val);
			}
		}
	}
}


point voxel_manager::interpolate_pt(float isolevel, point const &pt1, point const &pt2, float const val1, float const val2) const {

	if (abs(isolevel - val1) < TOLERANCE) return pt1;
	if (abs(isolevel - val2) < TOLERANCE) return pt2;
	if (abs(val1     - val2) < TOLERANCE) return pt1;
	float const mu(CLIP_TO_01((isolevel - val1) / (val2 - val1)));
	point pt;
	UNROLL_3X(pt[i_] = pt1[i_] + mu * (pt2[i_] - pt1[i_]););
	return pt;
}


void voxel_manager::get_triangles_for_voxel(vector<triangle> &triangles, unsigned x, unsigned y, unsigned z) const {

	float vals[8]; // 8 corner voxel values
	point pts[8]; // corner points
	unsigned cix(0);
	bool all_under_mesh(params.remove_under_mesh);
	assert(first_zval_above_mesh.size() == nx*ny);

	for (unsigned yhi = 0; yhi < 2; ++yhi) {
		for (unsigned xhi = 0; xhi < 2; ++xhi) {
			unsigned const xx(x+xhi), yy(y+yhi);
			float const xval(xx*vsz.x + lo_pos.x), yval(yy*vsz.y + lo_pos.y);
			if (all_under_mesh) all_under_mesh = (z < first_zval_above_mesh[yy*nx + xx]);
			
			for (unsigned zhi = 0; zhi < 2; ++zhi) {
				unsigned const zz(z+zhi), vix((xhi^yhi) + 2*yhi + 4*zhi);
				unsigned char const outside_val(outside.get(xx, yy, zz));
				if (outside_val) cix |= 1 << vix; // outside or on edge
				vals[vix] = (outside_val == 2) ? params.isolevel : get(xx, yy, zz); // check on_edge status
				pts [vix] = point(xval, yval, (zz*vsz.z + lo_pos.z));
			}
		}
	}
	assert(cix < 256);
	if (all_under_mesh) return;
	unsigned const edge_val(voxel_detail::edge_table[cix]);
	if (edge_val == 0)  return; // no polygons
	int const *const tris(voxel_detail::tri_table[cix]);
	point vlist[12];

	for (unsigned i = 0; i < 12; ++i) {
		if (!(edge_val & (1 << i))) continue;
		unsigned const *eix = voxel_detail::edge_to_vals[i];
		vlist[i] = interpolate_pt(params.isolevel, pts[eix[0]], pts[eix[1]], vals[eix[0]], vals[eix[1]]);
	}
	for (unsigned i = 0; tris[i] >= 0; i += 3) {
		triangle const tri(vlist[tris[i]], vlist[tris[i+1]], vlist[tris[i+2]]);
		if (tri.pts[0] == tri.pts[1] || tri.pts[0] == tri.pts[2] || tri.pts[1] == tri.pts[2]) continue; // invalid triangle
		#pragma omp critical(triangles_push_back)
		triangles.push_back(tri);
	}
}


void voxel_manager::calc_outside_val(unsigned x, unsigned y, unsigned z) {

	bool const on_edge(params.make_closed_surface && ((x == 0 || x == nx-1) || (y == 0 || y == ny-1) || (z == 0 || z == nz-1)));
	unsigned char const ival(on_edge? 2 : (((get(x, y, z) < params.isolevel) ^ params.invert) ? 1 : 0)); // on_edge is considered outside
	outside.set(x, y, z, ival);
}


void voxel_manager::determine_voxels_outside() { // determine inside/outside points

	assert(!empty());
	assert(vsz.x > 0.0 && vsz.y > 0.0 && vsz.z > 0.0);
	outside.init(nx, ny, nz, vsz, center, 0);
	first_zval_above_mesh.resize(nx*ny, 0);

	for (unsigned y = 0; y < ny; ++y) {
		for (unsigned x = 0; x < nx; ++x) {
			for (unsigned z = 0; z < nz; ++z) {
				calc_outside_val(x, y, z);
			}
			point const pos(get_pt_at(x, y, 0));
			int const xpos(get_xpos(pos.x)), ypos(get_xpos(pos.y));
			unsigned const zix(point_outside_mesh(xpos, ypos) ? 0 : max(0, int((mesh_height[ypos][xpos] - lo_pos.z)/vsz.z)));
			first_zval_above_mesh[y*nx + x] = zix;
		}
	}
}


void voxel_manager::remove_unconnected_outside() { // check for voxels connected to the mesh surface

	bool const keep_at_edge(params.keep_at_scene_edge == 1 || (params.keep_at_scene_edge == 2 && dynamic_mesh_scroll));
	remove_unconnected_outside_range(keep_at_edge, 0, 0, nx, ny);
}


void voxel_model::remove_unconnected_outside_block(unsigned block_ix) {

	unsigned const xbix(block_ix%NUM_BLOCKS), ybix(block_ix/NUM_BLOCKS);
	remove_unconnected_outside_range(1, xbix*xblocks, ybix*yblocks, min(nx, (xbix+1)*xblocks), min(ny, (ybix+1)*yblocks));
}


// outside: 0=inside, 1=outside, 2=on_edge, 4-bit set=anchored
void voxel_manager::remove_unconnected_outside_range(bool keep_at_edge, unsigned x1, unsigned y1, unsigned x2, unsigned y2) {

	vector<unsigned> work; // stack of voxels to process
	int const min_range[3] = {x1, y1, 0}, max_range[3] = {x2, y2, nz};

	// add voxels along the mesh surface
	for (int y = 0; y < MESH_Y_SIZE; ++y) {
		for (int x = 0; x < MESH_X_SIZE; ++x) {
			point const p(get_xval(x), get_yval(y), mesh_height[y][x]);
			int xyz[3];
			get_xyz(p, xyz);
			if (xyz[0] < (int)x1 || xyz[1] < (int)y1 || xyz[0] >= (int)x2 || xyz[1] >= (int)y2) continue; // out of range
			unsigned ix(0);
			if (!get_ix(p, ix) || outside[ix] != 0) continue; // off the voxel grid, outside, or on edge
			work.push_back(ix); // inside, anchored to the mesh
			outside[ix] |= 4; // mark as anchored
		}
	}

	// add voxels along the scene x/y boundary
	if (keep_at_edge) {
		for (unsigned y = y1; y < y2; ++y) {
			for (unsigned x = x1; x < x2; ++x) {
				if (x != x1 && x+1 != x2 && y != y1 && y+1 != y2) continue; // not on scene edge

				for (unsigned z = 0; z < nz; ++z) {
					unsigned const ix(outside.get_ix(x, y, z));
					if (outside[ix] == 1) continue; // outside
					work.push_back(ix); // inside, anchored to the mesh
					outside[ix] |= 4; // mark as anchored
				}
			}
		}
	}

	// flood fill anchored regions
	while (!work.empty()) {
		unsigned const cur(work.back());
		work.pop_back();
		assert(cur < outside.size());
		assert(outside[cur] & 4);
		unsigned const y(cur/(nz*nx)), cur_xz(cur - y*nz*nx), x(cur_xz/nz), z(cur_xz - x*nz);
		assert(outside.get_ix(x, y, z) == cur);

		for (unsigned dim = 0; dim < 3; ++dim) { // check neighbors
			for (unsigned dir = 0; dir < 2; ++dir) {
				int pos[3] = {x, y, z};
				pos[dim] += dir ? 1 : -1;
				if (pos[dim] < min_range[dim] || pos[dim] >= max_range[dim]) continue; // off the grid
				unsigned const ix(outside.get_ix(pos[0], pos[1], pos[2]));
				assert(ix < outside.size());
						
				if (outside[ix] == 0) { // inside
					work.push_back(ix);
					outside[ix] |= 4; // mark as anchored
				}
			}
		}
	} // while

	// if anchored or on_edge remove the anchored bit, else mark outside
	for (unsigned y = y1; y < y2; ++y) {
		for (unsigned x = x1; x < x2; ++x) {
			for (unsigned z = 0; z < nz; ++z) {
				unsigned const ix(outside.get_ix(x, y, z));
				outside[ix] = (outside[ix] & 6) ? (outside[ix] & 3) : 1;
			}
		}
	}
}


void voxel_manager::create_triangles(vector<triangle> &triangles) const {

	#pragma omp parallel for schedule(static,1)
	for (int y = 0; y < (int)ny-1; ++y) {
		for (unsigned x = 0; x < nx-1; ++x) {
			for (unsigned z = 0; z < nz-1; ++z) {
				get_triangles_for_voxel(triangles, x, y, z);
			}
		}
	}
}


void voxel_manager::get_triangles(vector<triangle> &triangles) {

	//RESET_TIME;
	determine_voxels_outside();
	if (params.remove_unconnected) remove_unconnected_outside();
	create_triangles(triangles);
	//PRINT_TIME("Voxels to Triangles");
}


bool voxel_manager::point_inside_volume(point const &pos) const {

	if (outside.empty()) return 0;
	unsigned ix(0);
	if (!outside.get_ix(pos, ix)) return 0; // off the voxel grid
	assert(ix < outside.size());
	return ((outside[ix]&3) == 0);
}


float voxel_model::get_ao_lighting_val(point const &pos) const {

	if (ao_lighting.empty()) return 1.0;
	unsigned ix(0);
	if (!ao_lighting.get_ix(pos, ix)) return 1.0; // off the voxel grid
	assert(ix < ao_lighting.size());
	return ao_lighting[ix];
}


unsigned voxel_model::get_block_ix(unsigned voxel_ix) const {

	assert(voxel_ix < size());
	unsigned const y(voxel_ix/(nz*nx)), vxz(voxel_ix - y*nz*nx), x(vxz/nz), bx(x/xblocks), by(y/yblocks);
	return by*NUM_BLOCKS + bx;
}


void voxel_model::clear() {

	for (unsigned i = 0; i < data_blocks.size(); ++i) {clear_block(i);} // unnecessary?
	free_context();
	tri_data.clear();
	data_blocks.clear();
	pt_to_ix.clear();
	modified_blocks.clear();
	ao_lighting.clear();
	voxel_manager::clear();
}


// returns true if something was cleared
bool voxel_model::clear_block(unsigned block_ix) {

	assert(block_ix < tri_data.size() && block_ix < data_blocks.size());
	bool const was_nonempty(!tri_data[block_ix].empty());
	tri_data[block_ix].clear();

	for (vector<int>::const_iterator i = data_blocks[block_ix].cids.begin(); i != data_blocks[block_ix].cids.end(); ++i) {
		remove_coll_object(*i);
	}
	data_blocks[block_ix].clear();
	return was_nonempty;
}


// returns the number of triangles created
unsigned voxel_model::create_block(unsigned block_ix, bool first_create) {

	assert(block_ix < tri_data.size() && block_ix < data_blocks.size());
	assert(tri_data[block_ix].empty() && data_blocks[block_ix].cids.empty());
	unsigned const xbix(block_ix%NUM_BLOCKS), ybix(block_ix/NUM_BLOCKS);
	vector<triangle> triangles;
	
	for (unsigned y = ybix*yblocks; y < min(ny-1, (ybix+1)*yblocks); ++y) {
		for (unsigned x = xbix*xblocks; x < min(nx-1, (xbix+1)*xblocks); ++x) {
			for (unsigned z = 0; z < nz-1; ++z) {
				get_triangles_for_voxel(triangles, x, y, z);
			}
		}
	}
	if (first_create) { // after the first creation pt_to_ix is out of order
		pt_to_ix[block_ix].pt = (point((xbix+0.5)*xblocks, (ybix+0.5)*yblocks, nz/2)*vsz + lo_pos);
		pt_to_ix[block_ix].ix = block_ix;
	}
	vertex_map_t<vertex_type_t> vmap(1);
	polygon_t poly(params.base_color);

	for (vector<triangle>::const_iterator i = triangles.begin(); i != triangles.end(); ++i) {
		poly.from_triangle(*i);
		tri_data[block_ix].add_poly(poly, vmap);
	}
	if (add_cobjs) {
		cobj_params cparams(params.elasticity, params.base_color, 0, 0, NULL, 0, params.tids[0]);
		cparams.is_model3d = 1;
		data_blocks[block_ix].cids.reserve(triangles.size());

		#pragma omp critical(add_coll_polygon)
		for (vector<triangle>::const_iterator t = triangles.begin(); t < triangles.end(); ++t) {
			int const cindex(add_coll_polygon(t->pts, 3, cparams, 0.0));
			assert(cindex >= 0 && (unsigned)cindex < coll_objects.size());
			//coll_objects[cindex].fixed = 1; // mark as fixed so that lmap cells will be generated
			data_blocks[block_ix].cids.push_back(cindex);
		}
	}
	return triangles.size();
}


void voxel_model::calc_ao_dirs() {

	if (!ao_dirs.empty()) return; // already calculated

	for (int y = -1; y <= 1; ++y) {
		for (int x = -1; x <= 1; ++x) {
			for (int z = -1; z <= 1; ++z) {
				if (x == 0 && y == 0 && z == 0) continue;
				vector3d const delta(x*vsz.x, y*vsz.y, z*vsz.z);
				unsigned const nsteps(max(1, int(params.ao_radius/delta.mag())));
				ao_dirs.push_back(step_dir_t(x, y, z, nsteps));
			}
		}
	}
}


void voxel_model::calc_ao_lighting_for_block(unsigned block_ix, bool increase_only) {

	assert(!ao_lighting.empty());
	float const norm(params.ao_weight_scale/ao_dirs.size());
	unsigned const xbix(block_ix%NUM_BLOCKS), ybix(block_ix/NUM_BLOCKS);
	
	#pragma omp parallel for schedule(static,1)
	for (int y = ybix*yblocks; y < (int)min(ny, (ybix+1)*yblocks); ++y) {
		for (unsigned x = xbix*xblocks; x < min(nx, (xbix+1)*xblocks); ++x) {
			for (unsigned z = 0; z < nz; ++z) {
				point const pos(ao_lighting.get_pt_at(x, y, z));
				if (!is_over_mesh(pos)) continue;
				
				if (pos.z + DZ_VAL < interpolate_mesh_zval(pos.x, pos.y, 0.0, 0, 1)) { // under mesh
					ao_lighting.set(x, y, z, 0);
					continue;
				}
				if (increase_only && ao_lighting.get(x, y, z) == 1.0) continue;
				float val(0.0);
				
				for (vector<step_dir_t>::const_iterator i = ao_dirs.begin(); i != ao_dirs.end(); ++i) {
					float cur_val(1.0);
					int cur[3] = {x, y, z};
					// bias to pos side by 1 unit for positive steps to help compensate for grid point vs. grid center alignments
					UNROLL_3X(if (i->dir[i_] > 0) cur[i_] += 1;);

					for (unsigned s = 0; s < i->nsteps; ++s) { // take steps in this direction
						UNROLL_3X(cur[i_] += i->dir[i_];); // increment first to skip the current voxel
						if (!is_valid_range(cur)) break; // stepped off the volume
						
						if (outside.get(cur[0], cur[1], cur[2]) == 0 || cur[2] < (int)first_zval_above_mesh[cur[1]*nx + cur[0]]) {
							cur_val = float(s)/float(i->nsteps);
							break; // voxel known to be inside the volume or under the mesh
						}
					}
					val += norm*cur_val;
					if (val >= 1.0) break;
				}
				ao_lighting.set(x, y, z, CLIP_TO_01(pow(val, params.ao_atten_power)));
			} // for z
		} // for x
	} // for y
}


void voxel_model::calc_ao_lighting() {

	if (empty() || scrolling) return; // too slow for scrolling
	if (params.ao_radius == 0.0 || params.ao_weight_scale == 0.0) return; // no AO lighting
	ao_lighting.init(nx, ny, nz, vsz, center, 1.0);
	calc_ao_dirs();

	for (unsigned block = 0; block < data_blocks.size(); ++block) {
		calc_ao_lighting_for_block(block, 0);
	}
}


// returns true if something was updated
bool voxel_model::update_voxel_sphere_region(point const &center, float radius, float val_at_center, int shooter, unsigned num_fragments) {

	assert(radius > 0.0);
	if (val_at_center == 0.0 || empty()) return 0;
	if (params.invert) val_at_center *= -1.0; // is this correct?
	unsigned const num[3] = {nx, ny, nz};
	unsigned bounds[3][2]; // {x,y,z} x {lo,hi}
	std::set<unsigned> blocks_to_update;
	float const dist_adjust(0.5*vsz.mag()); // single voxel diagonal half-width
	float const atten_thresh((params.invert ? 1.0 : -1.0)*params.atten_thresh);
	bool saw_inside(0), saw_outside(0);

	for (unsigned d = 0; d < 3; ++d) {
		bounds[d][0] = max(0, min((int)num[d]-1, int(floor(((center[d] - radius) - lo_pos[d])/vsz[d]))));
		bounds[d][1] = max(0, min((int)num[d]-1, int(ceil (((center[d] + radius) - lo_pos[d])/vsz[d]))));
	}
	for (unsigned y = bounds[1][0]; y <= bounds[1][1]; ++y) {
		for (unsigned x = bounds[0][0]; x <= bounds[0][1]; ++x) {
			bool was_updated(0);

			for (unsigned z = bounds[2][0]; z <= bounds[2][1]; ++z) {
				point const pos(get_pt_at(x, y, z));
				if (is_under_mesh(pos)) continue;
				float const dist(max(0.0f, (p2p_dist(center, pos) - dist_adjust)));
				if (dist >= radius) continue; // too far
				// update voxel values, linear falloff with distance from center (ending at 0.0 at radius)
				float &val(get_ref(x, y, z));
				float const prev_val(val);
				val += val_at_center*(1.0 - dist/radius);
				if (NORMALIZE_TO_1) val = max(-1.0f, min(1.0f, val));
				if (val == prev_val) continue; // no change
				if (params.atten_at_edges == 1) atten_edge_val(x, y, z, atten_thresh);
				if (params.atten_at_edges == 2) atten_top_val (x, y, z, atten_thresh);
				calc_outside_val(x, y, z);
				was_updated = 1;
				((val      < params.isolevel) ? saw_outside : saw_inside) = 1;
				((prev_val < params.isolevel) ? saw_outside : saw_inside) = 1;
			}
			if (!was_updated) continue;
			// check adjacent voxels since we will need to update our neighbors at the boundaries
			unsigned const bx1(max((int)x-1, 0        )/xblocks), by1(max((int)y-1, 0        )/yblocks);
			unsigned const bx2(min((int)x+1, (int)nx-1)/xblocks), by2(min((int)y+1, (int)ny-1)/yblocks);

			for (unsigned by = by1; by <= by2; ++by) {
				for (unsigned bx = bx1; bx <= bx2; ++bx) {
					unsigned const block_ix(by*NUM_BLOCKS + bx);
					assert(block_ix < data_blocks.size());
					blocks_to_update.insert(block_ix);
				}
			}
		}
	}
	if (!saw_inside || !saw_outside) return 0; // nothing else to do
	copy(blocks_to_update.begin(), blocks_to_update.end(), inserter(modified_blocks, modified_blocks.begin()));
	if (num_fragments == 0) return 1;
	float blend_val(fabs(eval_noise_texture_at(center)));
	blend_val = min(max(params.tex_mix_saturate*(blend_val - 0.5), -0.5), 0.5) + 0.5;
	colorRGBA color;
	blend_color(color, params.colors[1], params.colors[0], blend_val, 1);
	unsigned const tid(params.tids[blend_val > 0.5]);

	for (unsigned o = 0; o < num_fragments; ++o) {
		vector3d const velocity(signed_rand_vector(0.6));
		point fpos(center + signed_rand_vector_spherical(radius));
		gen_fragment(fpos, velocity, rand_uniform(1.0, 2.0), 0.5*rand_float(), color, tid, 1.0, shooter, 0);
	}
	return 1;
}


void voxel_model::proc_pending_updates() {

	RESET_TIME;
	if (modified_blocks.empty()) return;
	bool something_removed(0), something_added(0);
	vector<unsigned> blocks_to_update(modified_blocks.begin(), modified_blocks.end());
	modified_blocks.clear();
	
	for (unsigned i = 0; i < blocks_to_update.size(); ++i) {
		something_removed |= clear_block(blocks_to_update[i]);
	}
	if (something_removed) purge_coll_freed(0); // unecessary?

	if (something_removed && params.remove_unconnected) {
		for (unsigned i = 0; i < blocks_to_update.size(); ++i) {
			remove_unconnected_outside_block(blocks_to_update[i]);
		}
	}
	#pragma omp parallel for schedule(static,1)
	for (int i = 0; i < (int)blocks_to_update.size(); ++i) {
		something_added |= (create_block(blocks_to_update[i], 0) > 0);
	}
	if (something_added) build_cobj_tree(0, 0); // FIXME: inefficient - can we do a partial or delayed rebuild?
	if (!(something_added || something_removed)) return; // nothing updated

	if (!ao_lighting.empty()) {
		unsigned y_start(MESH_Y_SIZE), y_end(0); // start at denormalized range

		for (unsigned i = 0; i < blocks_to_update.size(); ++i) {
			unsigned const ybix(blocks_to_update[i]/NUM_BLOCKS), y1(ybix*yblocks), y2((ybix+1)*yblocks);
			y_start = min((unsigned)max(get_xpos(y1*vsz.y + lo_pos.y)-1, 0          ), y_start); // add a border of 1 to account for rounding errors
			y_end   = max((unsigned)min(get_xpos(y2*vsz.y + lo_pos.y)+1, MESH_Y_SIZE), y_end  );
			calc_ao_lighting_for_block(blocks_to_update[i], 1); // update can only remove, so lighting can only increase
		}
		if (y_start < y_end) update_smoke_indir_tex_y_range(y_start, y_end, 1);
	}
	scene_dlist_invalid = 1;
	PRINT_TIME("Process Voxel Updates");
}


void voxel_model::build(bool add_cobjs_) {

	RESET_TIME;
	add_cobjs = add_cobjs_;
	float const atten_thresh((params.invert ? 1.0 : -1.0)*params.atten_thresh);
	if (params.atten_at_edges == 1) atten_at_top_only(atten_thresh);
	if (params.atten_at_edges == 2) atten_at_edges   (atten_thresh);
	PRINT_TIME("  Atten at Top/Edges");
	determine_voxels_outside();
	PRINT_TIME("  Determine Voxels Outside");
	if (params.remove_unconnected) remove_unconnected_outside();
	PRINT_TIME("  Remove Unconnected");
	unsigned const tot_blocks(NUM_BLOCKS*NUM_BLOCKS);
	assert(pt_to_ix.empty() && tri_data.empty() && data_blocks.empty());
	pt_to_ix.resize(tot_blocks);
	data_blocks.resize(tot_blocks);
	tri_data.resize(tot_blocks, indexed_vntc_vect_t<vertex_type_t>(0));

	#pragma omp parallel for schedule(static,1)
	for (int block = 0; block < (int)tot_blocks; ++block) {
		create_block(block, 1);
	}
	PRINT_TIME("  Triangles to Model");
	calc_ao_lighting();
	PRINT_TIME("  Voxel AO Lighting");
}


void voxel_model::render(bool is_shadow_pass) { // not const because of vbo caching, etc.

	if (empty()) return; // nothing to do
	shader_t s;
	set_fill_mode();
	
	if (is_shadow_pass) {
		glDisable(GL_LIGHTING);
	}
	else if (!disable_shaders) {
		glDisable(GL_LIGHTING); // custom lighting calculations from this point on
		set_color_a(BLACK); // ambient will be set by indirect lighting in the shader
		float const min_alpha(0.5);
		noise_tex_gen.setup(NOISE_TSIZE, params.texture_rseed, 1.0, params.noise_freq);
		noise_tex_gen.bind_texture(5); // tu_id = 5
		set_multitex(0);
		setup_procedural_shaders(s, min_alpha, 1, 1, 1, 1, params.tex_scale, params.noise_scale, params.tex_mix_saturate);
		s.add_uniform_vector3d("tex_eval_offset", vector3d(DX_VAL*xoff2, DY_VAL*yoff2, 0.0));
		const char *cnames[2] = {"color0", "color1"};
		unsigned const tu_ids[2] = {0,8};

		for (unsigned i = 0; i < 2; ++i) {
			set_multitex(tu_ids[i]);
			select_texture(params.tids[i]);
			s.add_uniform_color(cnames[i], params.colors[i]);
		}
		set_multitex(0);
	}
	else {
		set_color_a(WHITE);
		glEnable(GL_TEXTURE_2D);
		glEnable(GL_ALPHA_TEST);
	}
	BLACK.do_glColor();
	set_color_d(params.base_color);
	float const spec(0.0), shine(1.0);
	set_specular(spec, shine);
	if (group_back_face_cull) glEnable(GL_CULL_FACE);
	sort(pt_to_ix.begin(), pt_to_ix.end(), comp_by_dist(get_camera_pos())); // sort near to far

	for (vector<pt_ix_t>::const_iterator i = pt_to_ix.begin(); i != pt_to_ix.end(); ++i) {
		if (DEBUG_BLOCKS) {
			const char *cnames[2] = {"color0", "color1"};
			for (unsigned d = 0; d < 2; ++d) {
				if (s.is_setup()) s.add_uniform_color(cnames[d], ((((i->ix & 1) != 0) ^ ((i->ix & NUM_BLOCKS) != 0)) ? RED : BLUE));
			}
		}
		assert(i->ix < tri_data.size());
		tri_data[i->ix].render(s, is_shadow_pass, GL_TRIANGLES);
	}
	if (s.is_setup()) {
		s.end_shader();
		disable_multitex_a();
	}
	if (group_back_face_cull) glDisable(GL_CULL_FACE);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_LIGHTING);
	set_specular(0.0, 1.0);
}


void voxel_model::free_context() {

	tri_data.free_vbos();
	noise_tex_gen.clear();
}


float voxel_model::eval_noise_texture_at(point const &pos) const {
	
	point const spos((pos + vector3d(DX_VAL*xoff2, DY_VAL*yoff2, 0.0))*params.noise_scale);
	point fpos;

	for (unsigned i = 0; i < 3; ++i) {
		int const ival((int)spos[i]);
		fpos[i] = spos[i] - ival; // create fractional part
		if (fpos[i] < 0.0) fpos[i] += 1.0; // make positive
		if ((spos[i] < 0.0) ^ (ival & 1)) fpos[i] = 1.0 - fpos[i]; // apply mirroring
		fpos[i] = fpos[i] - 0.5; // transform from [0,1] to [-0.5,0.5]
	}
	return noise_tex_gen.eval_at(fpos*NOISE_TSIZE);
}


void gen_voxel_landscape() {

	RESET_TIME;
	bool const add_cobjs(1);
	unsigned const nx(MESH_X_SIZE), ny(MESH_Y_SIZE), nz(max((unsigned)MESH_Z_SIZE, (nx+ny)/4));
	//float const zlo(zbottom), zhi(max(ztop, zlo + Z_SCENE_SIZE)); // Note: does not include czmin/czmax range
	float const zlo(zbottom), zhi(min(czmin, zbottom) + Z_SCENE_SIZE); // Note: matches zhi of 3D volume textures/matrices for lighting
	// slightly smaller than 2.0 to avoid z-fighting issues at the edge of the mesh/water
	float const ssz_xy_mult(2.0*(1.0 - 0.1/(MESH_X_SIZE + MESH_Y_SIZE)));
	vector3d const vsz(ssz_xy_mult*X_SCENE_SIZE/nx, ssz_xy_mult*Y_SCENE_SIZE/ny, (zhi - zlo)/nz);
	point const center(0.0, 0.0, 0.5*(zlo + zhi));
	vector3d const gen_offset(DX_VAL*xoff2, DY_VAL*yoff2, 0.0);
	terrain_voxel_model.set_params(global_voxel_params);
	terrain_voxel_model.clear();
	terrain_voxel_model.init(nx, ny, nz, vsz, center, 0.0);
	terrain_voxel_model.create_procedural(global_voxel_params.mag, global_voxel_params.freq, gen_offset,
		NORMALIZE_TO_1, global_voxel_params.geom_rseed, 456+rand_gen_index);
	PRINT_TIME(" Voxel Gen");
	terrain_voxel_model.build(add_cobjs);
	PRINT_TIME(" Voxels to Triangles/Cobjs");
}


void voxel_file_err(string const &str, int &error) {
	cout << "Error reading voxel config option " << str << "." << endl;
	error = 1;
}


bool parse_voxel_option(FILE *fp) {

	int error(0);
	char strc[MAX_CHARS] = {0};

	if (!read_str(fp, strc)) return 0;
	string const str(strc);

	if (str == "mag") {
		if (!read_float(fp, global_voxel_params.mag)) voxel_file_err("mag", error);
	}
	else if (str == "freq") {
		if (!read_float(fp, global_voxel_params.freq)) voxel_file_err("freq", error);
	}
	else if (str == "isolevel") {
		if (!read_float(fp, global_voxel_params.isolevel)) voxel_file_err("isolevel", error);
	}
	else if (str == "elasticity") {
		if (!read_float(fp, global_voxel_params.elasticity) || global_voxel_params.elasticity < 0.0) voxel_file_err("elasticity", error);
	}
	else if (str == "tex_scale") {
		if (!read_float(fp, global_voxel_params.tex_scale)) voxel_file_err("tex_scale", error);
	}
	else if (str == "noise_scale") {
		if (!read_float(fp, global_voxel_params.noise_scale)) voxel_file_err("noise_scale", error);
	}
	else if (str == "noise_freq") {
		if (!read_float(fp, global_voxel_params.noise_freq)) voxel_file_err("noise_freq", error);
	}
	else if (str == "tex_mix_saturate") {
		if (!read_float(fp, global_voxel_params.tex_mix_saturate)) voxel_file_err("tex_mix_saturate", error);
	}
	else if (str == "ao_radius") {
		if (!read_float(fp, global_voxel_params.ao_radius) || global_voxel_params.ao_radius < 0.0) voxel_file_err("ao_radius", error);
	}
	else if (str == "ao_weight_scale") {
		if (!read_float(fp, global_voxel_params.ao_weight_scale) || global_voxel_params.ao_weight_scale <= 0.0) voxel_file_err("ao_weight_scale", error);
	}
	else if (str == "ao_atten_power") {
		if (!read_float(fp, global_voxel_params.ao_atten_power) || global_voxel_params.ao_atten_power <= 0.0) voxel_file_err("ao_atten_power", error);
	}
	else if (str == "invert") {
		if (!read_bool(fp, global_voxel_params.invert)) voxel_file_err("invert", error);
	}
	else if (str == "make_closed_surface") {
		if (!read_bool(fp, global_voxel_params.make_closed_surface)) voxel_file_err("make_closed_surface", error);
	}
	else if (str == "remove_unconnected") {
		if (!read_bool(fp, global_voxel_params.remove_unconnected)) voxel_file_err("remove_unconnected", error);
	}
	else if (str == "keep_at_scene_edge") {
		if (!read_uint(fp, global_voxel_params.keep_at_scene_edge) || global_voxel_params.keep_at_scene_edge > 2) voxel_file_err("keep_at_scene_edge", error);
	}
	else if (str == "remove_under_mesh") {
		if (!read_bool(fp, global_voxel_params.remove_under_mesh)) voxel_file_err("remove_under_mesh", error);
	}
	else if (str == "atten_at_edges") {
		if (!read_uint(fp, global_voxel_params.atten_at_edges) || global_voxel_params.atten_at_edges > 2) voxel_file_err("atten_at_edges", error);
	}
	else if (str == "atten_thresh") {
		if (!read_float(fp, global_voxel_params.atten_thresh) || global_voxel_params.atten_thresh <= 0.0) voxel_file_err("atten_thresh", error);
	}
	else if (str == "geom_rseed") {
		if (!read_int(fp, global_voxel_params.geom_rseed)) voxel_file_err("geom_rseed", error);
	}
	else if (str == "texture_rseed") {
		if (!read_int(fp, global_voxel_params.texture_rseed)) voxel_file_err("texture_rseed", error);
	}
	else if (str == "tid1") {
		if (!read_str(fp, strc)) voxel_file_err("tid1", error);
		global_voxel_params.tids[0] = get_texture_by_name(std::string(strc));
	}
	else if (str == "tid2") {
		if (!read_str(fp, strc)) voxel_file_err("tid2", error);
		global_voxel_params.tids[1] = get_texture_by_name(std::string(strc));
	}
	else if (str == "base_color") {
		if (!read_color(fp, global_voxel_params.base_color)) voxel_file_err("base_color", error);
	}
	else if (str == "color1") {
		if (!read_color(fp, global_voxel_params.colors[0])) voxel_file_err("color1", error);
	}
	else if (str == "color2") {
		if (!read_color(fp, global_voxel_params.colors[1])) voxel_file_err("color2", error);
	}
	else {
		cout << "Unrecognized voxel keyword in input file: " << str << endl;
		error = 1;
	}
	return !error;
}


void render_voxel_data(bool shadow_pass) {
	terrain_voxel_model.render(shadow_pass);
}

void free_voxel_context() {
	terrain_voxel_model.free_context();
}

bool point_inside_voxel_terrain(point const &pos) {
	return terrain_voxel_model.point_inside_volume(pos);
}

float get_voxel_terrain_ao_lighting_val(point const &pos) {
	return terrain_voxel_model.get_ao_lighting_val(pos);
}

bool update_voxel_sphere_region(point const &center, float radius, float val_at_center, int shooter, unsigned num_fragments) {
	return terrain_voxel_model.update_voxel_sphere_region(center, radius, val_at_center, shooter, num_fragments);
}

void proc_voxel_updates() {
	terrain_voxel_model.proc_pending_updates();
}


