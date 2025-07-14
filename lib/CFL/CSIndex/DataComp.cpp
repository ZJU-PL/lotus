/*
*  DataComp is a data compression algorithm.
*/

#include "CFL/CSIndex/DataComp.h"

DataComp::DataComp(vector<vector<int> > _data):data(_data) {
	srand48(time(NULL));
	num_cluster = 1;
	comp_data = vector<vector<int> >(data.size(),vector<int>());
	max_length = 0;
	int sum = 0;
	valid_num = 0;
	threshold = 4;
	for (int i = 0; i < data.size(); i++) {
		sum += data[i].size();
		if (data[i].size() > max_length)
			max_length = data[i].size();
		if (data[i].size() >= threshold)
			valid_num++;
	}
	avg_length = sum/data.size();
	classid = vector<int>(data.size(),-1);
	orgsize = sum;
}

DataComp::DataComp(vector<vector<int> > _data, vector<int> _grts, int _K, int _max)
	:data(_data),order(_grts),num_cluster(_K),max_num(_max) {
	srand48(time(NULL));
	comp_data = vector<vector<int> >(data.size(),vector<int>());
	max_length = 0;
	int sum = 0;
	valid_num = 0;
	threshold = 4;
	for (int i = 0; i < data.size(); i++) {
		sum += data[i].size();
		if (data[i].size() > max_length)
			max_length = data[i].size();
		if (data[i].size() >= threshold)
			valid_num++;
	}
	avg_length = sum/data.size();
	classid = vector<int>(data.size(),-1);
	orgsize = sum;
}

DataComp::DataComp(vector<vector<int> > _data, int _K):data(_data),num_cluster(_K) {
	srand48(time(NULL));
	comp_data = vector<vector<int> >(data.size(),vector<int>());
	max_length = 0;
	int sum = 0;
	valid_num = 0;
	threshold = 4;
	for (int i = 0; i < data.size(); i++) {
		sum += data[i].size();
		if (data[i].size() > max_length)
			max_length = data[i].size();
		if (data[i].size() >= threshold)
			valid_num++;
	}
	avg_length = sum/data.size();
	classid = vector<int>(data.size(),-1);
	orgsize = sum;
}

void DataComp::slidewin_heu(vector<vector<int> >& pdata, vector<int>& grts, 
		map<int,vector<int> >& table, bool saved) {
	vector<int>::iterator vit;
	vector<int> com_set;
	vector<int> cur_cv;
	
	for (int i = 0; i < grts.size(); i++) {
		sort(pdata[i].begin(),pdata[i].end());
	}
	
	bool restart = false;
	int ind = 0, newid=-1;
	int thresh = 0, size_limit = 4;
	for (; ind < grts.size(); ind++) {
		if (pdata[grts[ind]].size()>=size_limit-1) {
			cur_cv.push_back(grts[ind]);
			break;
		}
	}
	while (ind<grts.size() && pdata[grts[ind]].size()<size_limit)
		ind++;
		
	while (ind < grts.size()) {
		restart = false;

		if (cur_cv.size()==1) {
			vector<int> tmp_set;
			set_intersection(pdata[cur_cv[0]].begin(),pdata[cur_cv[0]].end(),
				pdata[grts[ind]].begin(),pdata[grts[ind]].end(),back_inserter(tmp_set));
			if (tmp_set.size()>=3) {
				com_set = tmp_set;
				cur_cv.push_back(grts[ind]);
				ind++;
				while (ind<grts.size() && pdata[grts[ind]].size()<size_limit)
					ind++;
			}
			else 
				restart = true;
		}
		else {
			vector<int> tmp_set;
			set_intersection(com_set.begin(),com_set.end(),pdata[grts[ind]].begin(),pdata[grts[ind]].end(),
				back_inserter(tmp_set));
			if (cur_cv.size()*(com_set.size()-tmp_set.size())<com_set.size()-1-thresh) {
				com_set = tmp_set;
				cur_cv.push_back(grts[ind]);
				ind++;
				while (ind<grts.size() && pdata[grts[ind]].size()<size_limit)
					ind++;
			}
			else
				restart = true;
		}
		
		if(restart) {
			if (com_set.size()>0) {
				vector<int> tmp_vec;
				table[newid] = com_set;
				for (vit = cur_cv.begin(); vit != cur_cv.end(); vit++) {
					tmp_vec.clear();
					set_difference(pdata[*vit].begin(),pdata[*vit].end(),
						com_set.begin(),com_set.end(),back_inserter(tmp_vec));
					if (saved) {
						pdata[*vit] = tmp_vec;
						pdata[*vit].push_back(newid);
					}
				}
				newid--;
			}
			
			// clear utility pdata structure
			com_set.clear();
			cur_cv.clear();
			if (ind<grts.size()) {
				cur_cv.push_back(grts[ind]);
				ind++;
				while (ind<grts.size() && pdata[grts[ind]].size()<size_limit)
					ind++;
			}
		}
	}
	
	// process last cur_cv
	if (com_set.size()>0) {
		vector<int> tmp_vec;
		table[newid] = com_set;
		for (vit = cur_cv.begin(); vit != cur_cv.end(); vit++) {
			vector<int>().swap(tmp_vec);
			set_difference(pdata[*vit].begin(),pdata[*vit].end(),
				com_set.begin(),com_set.end(),back_inserter(tmp_vec));
			if (saved) {
				pdata[*vit] = tmp_vec;
				pdata[*vit].push_back(newid);
			}
		}	
	}	
}

void DataComp::comp_swin() {
	slidewin_heu(data,order,comp_table,true);
}

void DataComp::init_classid() {
	map<int,vector<int> > table;
	vector<int>::iterator vit;
	vector<int> com_set;
	vector<int> cur_cv;
	
	// for test
	map<int,int> win_len;
	
	for (int i = 0; i < order.size(); i++) {
		sort(data[i].begin(),data[i].end());
	}
	
	bool restart = false;
	int ind = 0, newid=-1;
	int thresh = 0, size_limit = 4;
	for (; ind < order.size(); ind++) {
		if (data[order[ind]].size()>=size_limit) {
			cur_cv.push_back(order[ind]);
			break;
		}
	}
	ind++;
	while (ind<order.size() && data[order[ind]].size()<size_limit)
		ind++;
		
	int cind = 0;
	while (ind < order.size()) {
		restart = false;

		if (cur_cv.size()==1) {
			vector<int> tmp_set;
			set_intersection(data[cur_cv[0]].begin(),data[cur_cv[0]].end(),
				data[order[ind]].begin(),data[order[ind]].end(),back_inserter(tmp_set));
			// for test
//			cout << "check " << cur_cv[0] << " and " << order[ind] << endl;
			if (tmp_set.size()>=3) {
				com_set = tmp_set;
				cur_cv.push_back(order[ind]);
				ind++;
				while (ind<order.size() && data[order[ind]].size()<size_limit)
					ind++;
			}
			else 
				restart = true;
		}
		else {
			vector<int> tmp_set;
			set_intersection(com_set.begin(),com_set.end(),data[order[ind]].begin(),data[order[ind]].end(),
				back_inserter(tmp_set));
			if (cur_cv.size()*(com_set.size()-tmp_set.size())<com_set.size()-1-thresh) {
				com_set = tmp_set;
				cur_cv.push_back(order[ind]);
				ind++;
				while (ind<order.size() && data[order[ind]].size()<size_limit)
					ind++;
			}
			else
				restart = true;
		}
		
		if(restart) {
//			vector<int> tmp_vec;
//			table[newid] = com_set;
			for (vit = cur_cv.begin(); vit != cur_cv.end(); vit++) {
				/*
				tmp_vec.clear();
				set_difference(data[*vit].begin(),data[*vit].end(),
					com_set.begin(),com_set.end(),back_inserter(tmp_vec));
				if (saved) {
					data[*vit] = tmp_vec;
					data[*vit].push_back(newid);
				}
				*/
				classid[*vit] = cind;
			}
			cind++;
//			newid--;

			// for test
			win_len[cur_cv.size()]++;
			
			// clear utility data structure
			com_set.clear();
			cur_cv.clear();
			if (ind<order.size()) {
				cur_cv.push_back(order[ind]);
				ind++;
				while (ind<order.size() && data[order[ind]].size()<size_limit)
					ind++;
			}
		}
	}
	
	// process last cur_cv
//	if (com_set.size()>0) {
//		vector<int> tmp_vec;
//		table[newid] = com_set;
		for (vit = cur_cv.begin(); vit != cur_cv.end(); vit++) {
			/*
			vector<int>().swap(tmp_vec);
			set_difference(data[*vit].begin(),data[*vit].end(),
				com_set.begin(),com_set.end(),back_inserter(tmp_vec));
			if (saved) {
				data[*vit] = tmp_vec;
				data[*vit].push_back(newid);
			}
			*/
			classid[*vit] = cind;
		}
		cind++;
//	}	
	num_cluster = cind;
	centroids = vector<vector<int> >(num_cluster,vector<int>());
	
	win_len[cur_cv.size()]++;
	
	// for test
	/*
	cout << "init num clsuter = " << num_cluster << endl;
	
	cout << "===========================classid=====================" << endl;
	for (int i = 0;  i < classid.size(); i++) {
		if (data[order[i]].size()<4) continue;
		cout << "[" << order[i] << "\t" << classid[order[i]] << "] " << endl;
	}
	cout << endl;
	
	cout << "===========================window length=====================" << endl;
	map<int,int>::iterator mit;
	for (mit = win_len.begin(); mit != win_len.end(); mit++)
		cout << "wlen " << mit->first << " : " << mit->second << endl;
	cout << endl;
	*/
}

void DataComp::init_centroids() {
	map<int,vector<int> > tmp_table;
	slidewin_heu(data,order,tmp_table,false);
	
	vector<int> porder;
	vector<vector<int> > pdata;
	map<int,vector<int> >::iterator mit;
	int k = 0, comp_iter = 0;
	for (k = 0, mit = tmp_table.begin(); mit != tmp_table.end(); mit++,k++) {
		pdata.push_back(mit->second);
		porder.push_back(k);
	}
	sort(pdata.begin(), pdata.end(), mycomp());
	if (pdata.size()<num_cluster) 
		num_cluster = (int)(pdata.size()*1);
	
	// randomly select num_cluster centroids;
	set<int> index;
	vector<vector<int> >().swap(centroids);
	while (true) {
		int ind = lrand48()%pdata.size();
		index.insert(ind);
		if (index.size() == num_cluster) break;
	}
	set<int>::iterator sit;
	for (sit = index.begin(); sit != index.end(); sit++) 
		centroids.push_back(pdata[*sit]);

	// for test
//	display_centroids();
}

void DataComp::assign_class() {
	double distance, min_dist;
	map<int,int> cl_num;
	for (int i = 0; i < data.size(); i++) {
		if (data[i].size()<threshold) continue;
		cl_num[classid[i]]++;
	}
	for (int i = 0; i < data.size(); i++) {
		if (data[i].size()<threshold) continue;
		min_dist = 100000000;
		int cid = classid[i];
		for (int j = 0; j < num_cluster; j++) {
			vector<int> tmp_vec;
			set_symmetric_difference(data[i].begin(),data[i].end(),
					centroids[j].begin(),centroids[j].end(),back_inserter(tmp_vec));
			distance = tmp_vec.size()+(cl_num[cid]*1.0)/(valid_num*1.0)*centroids[cid].size();
			if (min_dist > distance) {
				min_dist = distance;
				classid[i] = j;
			}
		}
	}
}

void DataComp::update_centroids(bool shrink) {
	int element, cid;
	vector<map<int,int> > vecmap = vector<map<int,int> >(num_cluster);
	vector<int> num_cl = vector<int>(num_cluster,0);
	for (int i = 0; i < data.size(); i++) {
		if (data[i].size()<threshold) continue;
		cid = classid[i];

		for (int j = 0; j < data[i].size(); j++) {
			element = data[i][j];
			vecmap[cid][element] = vecmap[cid][element]+1;
		}
		num_cl[cid]++;
	}
	
	map<int,int>::iterator mit;
	for (int i = 0; i < num_cluster; i++) {
		centroids[i].clear();
		for (mit = vecmap[i].begin(); mit != vecmap[i].end(); mit++) {
			if (mit->second <= num_cl[i]*0.5) continue;
			/*
			int tol = lrand48()%max_length;
			if (tol < avg_length) continue;
			*/
			centroids[i].push_back(mit->first);
		}	
	}
	
	// shrink empty centroids
	if (shrink) {
		vector<vector<int> > tmp_vec;
		for (int i = 0; i < num_cluster; i++) {
			if (centroids[i].size()<=0) continue;
			tmp_vec.push_back(centroids[i]);
		}
		centroids.clear();
		centroids = tmp_vec;
		num_cluster = tmp_vec.size();
	}

	// for test
	/*
	cout << "\n-------------------------------------------------" << endl;
	display_centroids();
	*/
}

void DataComp::gen_result() {
	int index = -1, cid = 0;
	
	map<int,vector<int> > cl_num;
	for (int i = 0; i < data.size(); i++) {
		if (data[i].size()<threshold) continue;
		cid = classid[i];
		cl_num[cid].push_back(i);
	}
	
	comp_table.clear();
	map<int,int> index_map;
	for (int i = 0; i < num_cluster; i++) {
		if (cl_num[i].size()<2) continue;
		sort(centroids[i].begin(),centroids[i].end());
		comp_table[index] = centroids[i];
		index_map[i] = index;
		index--;
	}

	for (int i = 0; i < data.size(); i++) {
		if (data[i].size()<threshold) {
			comp_data[i] = data[i];
			continue;
		}
	
		cid = classid[i];
		if (cl_num[cid].size()<2 || comp_table[index_map[cid]].size()==0) {
			comp_data[i] = data[i];
			continue;
		}
		
		vector<int> tmp_vec;
		set_difference(data[i].begin(),data[i].end(),centroids[cid].begin(),centroids[cid].end(),back_inserter(tmp_vec));
		comp_data[i] = tmp_vec;
		tmp_vec.clear();
		set_difference(centroids[cid].begin(),centroids[cid].end(),data[i].begin(),data[i].end(),back_inserter(tmp_vec));
		for (int j = 0; j < tmp_vec.size(); j++) {
			comp_data[i].push_back(-(max_num+tmp_vec[j]));
		}
		comp_data[i].push_back(index_map[cid]);
	}
}

void DataComp::comp_kmeans() {
//	init_centroids();
//	assign_class();
	init_classid();	
	update_centroids(true);
	cout << "complete initialization num_cluster=" << num_cluster << endl;

//	if (true) exit(0);		
	
	int iter = 0, max_iter = 5; // default is 5
	bool change = false;
	vector<int> old_cid;
	while (true && num_cluster<15000) {
		old_cid = classid;
		cout << "begin assign class" << endl;
		assign_class();
		cout << "complete assign class" << endl;
		update_centroids(true);
		cout << "complete update" << endl;
		
		iter++;
		change = false;
		for (int i = 0; i < num_cluster; i++) {
			if (old_cid[i] != classid[i]) {
				change = true;
				break;
			}
		}
		if (!change || iter>=max_iter) break;
		cout << "iter " << iter << "\t";
	}
	/*
	if (iter>0) {
	assign_class();
	update_centroids(false);
	}
	*/
	gen_result();
}

int DataComp::getSize() {
	int size = 0;
	for (int i = 0; i < comp_data.size(); i++)
		size += comp_data[i].size();
	map<int,vector<int> >::iterator mit;
	for (mit = comp_table.begin(); mit != comp_table.end(); mit++)
		size += mit->second.size();
	return size;
}

bool DataComp::checkSize() {
	int newsize = getSize();
	return (orgsize>newsize);
}

void DataComp::getcomp_data(vector<vector<int> >& cdata) {
	cdata = comp_data;
}

void DataComp::getcomp_table(map<int,vector<int> >& ctable) {
	ctable = comp_table;
}

void DataComp::display_centroids() {
	// for test
	cout << "display centroids" << endl;
	for (int i = 0; i < centroids.size(); i++) {
		cout << i << ": ";
		for (int j = 0; j < centroids[i].size(); j++)
			cout << centroids[i][j] << " ";
		cout << endl;
	}
	cout << endl;
}

void DataComp::display_compdata() {
	cout << "Compressed data" << endl;
	for (int i = 0; i < comp_data.size(); i++) {
		if (comp_data[order[i]].size()==0) continue;
		cout << order[i] << ": ";
		for (int j = 0; j < comp_data[order[i]].size(); j++)
			cout << comp_data[order[i]][j] << " ";
		cout << endl;
	}
}

void DataComp::display_comptable() {
	cout << "Coding Table comp_table size=" << comp_table.size() << endl;
	vector<int>::iterator vit;
	map<int,vector<int> >::iterator mit;
	for (mit = comp_table.begin(); mit != comp_table.end(); mit++) {
		cout << mit->first << ": ";
		for (vit = mit->second.begin(); vit != mit->second.end(); vit++)
			cout << *vit << " ";
		cout << endl;
	}
	cout << endl;
}
