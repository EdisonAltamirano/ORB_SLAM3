/**
* This file is part of ORB-SLAM3
*
* Copyright (C) 2017-2021 Carlos Campos, Richard Elvira, Juan J. Gómez Rodríguez, José M.M. Montiel and Juan D. Tardós, University of Zaragoza.
* Copyright (C) 2014-2016 Raúl Mur-Artal, José M.M. Montiel and Juan D. Tardós, University of Zaragoza.
*
* ORB-SLAM3 is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
* License as published by the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* ORB-SLAM3 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
* the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with ORB-SLAM3.
* If not, see <http://www.gnu.org/licenses/>.
*/

#include "MapDrawer.h"
#include "MapPoint.h"
#include "KeyFrame.h"
#include <pangolin/pangolin.h>
#include <mutex>

namespace ORB_SLAM3
{


MapDrawer::MapDrawer(Atlas* pAtlas, const string &strSettingPath, Settings* settings):
    mpAtlas(pAtlas),    
    m_octree(NULL),
    m_maxRange(-1.0),
    m_useHeightMap(true),
    m_res(0.05),
    m_colorFactor(0.8),
    m_treeDepth(0),
    m_maxTreeDepth(0)
{
    if(settings){
        newParameterLoader(settings);
    }
    else{
        cv::FileStorage fSettings(strSettingPath, cv::FileStorage::READ);
        bool is_correct = ParseViewerParamFile(fSettings);

        if(!is_correct)
        {
            std::cerr << "**ERROR in the config file, the format is not correct**" << std::endl;
            try
            {
                throw -1;
            }
            catch(exception &e)
            {

            }
        }
    }
    m_octree = new octomap::ColorOcTree(m_res);
    // initialize octomap
    m_octree->setClampingThresMin(0.12);
    m_octree->setClampingThresMax(0.97);
    m_octree->setProbHit(0.7);
    m_octree->setProbMiss(0.4);

    m_treeDepth = m_octree->getTreeDepth();
    m_maxTreeDepth = m_treeDepth;

}

void MapDrawer::newParameterLoader(Settings *settings) {
    mKeyFrameSize = settings->keyFrameSize();
    mKeyFrameLineWidth = settings->keyFrameLineWidth();
    mGraphLineWidth = settings->graphLineWidth();
    mPointSize = settings->pointSize();
    mCameraSize = settings->cameraSize();
    mCameraLineWidth  = settings->cameraLineWidth();
}
//Octomap
void MapDrawer::LoadOctoMap()
{
    octomap::AbstractOcTree* tree = octomap::AbstractOcTree::read("octomap.ot");
    m_octree= dynamic_cast<octomap::ColorOcTree*> (tree);
}

void MapDrawer::UpdateOctomap(vector<KeyFrame*> vKFs)
{
    int N = vKFs.size();
    for ( size_t i=lastKeyframeSize; i<N ; i++ )
    {
         Eigen::Isometry3d pose = ORB_SLAM2::Converter::toSE3Quat( vKFs[i]->GetPose());

         pcl::PointCloud<pcl::PointXYZRGB>  ground;
         pcl::PointCloud<pcl::PointXYZRGB>  nonground;

         GeneratePointCloud( vKFs[i], ground, nonground);
         octomap::point3d sensorOrigin = octomap::point3d( pose(0,3), pose(1,3), pose(2,3));
         InsertScan(sensorOrigin, ground, nonground);
    }

     lastKeyframeSize = N;
}

void MapDrawer::GeneratePointCloud(KeyFrame *kf, pcl::PointCloud<pcl::PointXYZRGB> &ground, pcl::PointCloud<pcl::PointXYZRGB> &nonground)
{
    pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud (new pcl::PointCloud<pcl::PointXYZRGB>);
    for ( int m=0; m<(kf->mImDep.rows); m+=1 )
     {
          for ( int n=0; n<(kf->mImDep.cols); n+=1 )
          {
              float d = kf->mImDep.ptr<float>(m)[n];///1000.0;
              if (d < 0.01 || d>2.0)
                 continue;
              pcl::PointXYZRGB p;
              p.z = d;
              p.x = ( n - cx) * p.z / fx;
              p.y = ( m - cy) * p.z / fy;
              if(p.y<-3.0 || p.y>3.0) continue;
              p.b = kf->mImRGB.ptr<uchar>(m)[n*3];
              p.g = kf->mImRGB.ptr<uchar>(m)[n*3+1];
              p.r = kf->mImRGB.ptr<uchar>(m)[n*3+2];
              cloud->points.push_back( p );

          }
     }
    pcl::VoxelGrid<pcl::PointXYZRGB>vg;
    vg.setInputCloud(cloud);
    vg.setLeafSize(0.01,0.01, 0.01);
    vg.filter(*cloud);
    //STEP2: transform camera to word
    Eigen::Isometry3d T = ORB_SLAM2::Converter::toSE3Quat( kf->GetPose() );
    pcl::PointCloud<pcl::PointXYZRGB> temp;
    pcl::transformPointCloud( *cloud, temp, T.inverse().matrix());

       // filter ground plane
    if(temp.size()<50)
    {
        printf("pointcloud too small skip ground plane extraction\n;");
        ground = temp;
    }
    else
    {
        pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients);
        pcl::PointIndices::Ptr inliers(new pcl::PointIndices);

        pcl::SACSegmentation<pcl::PointCloud<pcl::PointXYZRGB>::PointType> seg;
        seg.setOptimizeCoefficients(true);
        seg.setModelType(pcl::SACMODEL_PERPENDICULAR_PLANE);
        seg.setMethodType(pcl::SAC_RANSAC);
        seg.setMaxIterations(200);
        seg.setDistanceThreshold(0.04);
        seg.setAxis(Eigen::Vector3f(0, 1 ,0));
        seg.setEpsAngle(0.5);

        pcl::PointCloud<pcl::PointXYZRGB> cloud_filtered(temp);
        pcl::ExtractIndices<pcl::PointCloud<pcl::PointXYZRGB>::PointType> extract;
        bool groundPlaneFound = false;
        while(cloud_filtered.size()>10 && !groundPlaneFound)
        {
            seg.setInputCloud(cloud_filtered.makeShared());
            seg.segment(*inliers, *coefficients);
            if(inliers->indices.size()==0)
            {
                break;
            }
            extract.setInputCloud(cloud_filtered.makeShared());
            extract.setIndices(inliers);
            if (std::abs(coefficients->values.at(3)) >0.07)
            {
                printf("Ground plane found: %zu/%zu inliers. Coeff: %f %f %f %f", inliers->indices.size(),
                                                                                  cloud_filtered.size(),
                                                                                  coefficients->values.at(0),
                                                                                  coefficients->values.at(1),
                                                                                  coefficients->values.at(2),
                                                                                  coefficients->values.at(3));
                extract.setNegative (false);
                extract.filter (ground);
                // remove ground points from full pointcloud:
                // workaround for PCL bug:
                if(inliers->indices.size() != cloud_filtered.size())
                {
                  extract.setNegative(true);
                  pcl::PointCloud<pcl::PointXYZRGB> cloud_out;
                  extract.filter(cloud_out);
                  nonground += cloud_out;
                  cloud_filtered = cloud_out;
                }

                groundPlaneFound = true;
            }
            else
            {
                printf("Horizontal plane (not ground) found: %zu/%zu inliers. Coeff: %f %f %f %f", inliers->indices.size(),
                                                                                                   cloud_filtered.size(),
                                                                                                   coefficients->values.at(0),
                                                                                                   coefficients->values.at(1),
                                                                                                   coefficients->values.at(2),
                                                                                                   coefficients->values.at(3));
                pcl::PointCloud<pcl::PointXYZRGB> cloud_out;
                extract.setNegative (false);
                extract.filter(cloud_out);
                nonground +=cloud_out;
                if(inliers->indices.size() != cloud_filtered.size())
                {
                     extract.setNegative(true);
                     cloud_out.points.clear();
                     extract.filter(cloud_out);
                     cloud_filtered = cloud_out;
                 }
                else
                {
                     cloud_filtered.points.clear();

                }
              }

         }//while

         if(!groundPlaneFound)
         {
             nonground = temp;
         }
    }
}

void MapDrawer::InsertScan(octomap::point3d sensorOrigin, pcl::PointCloud<pcl::PointXYZRGB> &ground, pcl::PointCloud<pcl::PointXYZRGB> &nonground)
{

    if(!m_octree->coordToKeyChecked(sensorOrigin, m_updateBBXMin)|| !m_octree->coordToKeyChecked(sensorOrigin, m_updateBBXMax))
     {
            printf("coulde not generate key for origin\n");
     }
     octomap::KeySet free_cells, occupied_cells;
     for(auto p:ground.points)
     {
        octomap::point3d point(p.x, p.y, p.z);
        // only clear space (ground points)
        if(m_octree->computeRayKeys(sensorOrigin, point, m_keyRay))
        {
             free_cells.insert(m_keyRay.begin(), m_keyRay.end());
             m_octree->averageNodeColor(p.x, p.y, p.z, p.r,p.g, p.b);
        }
        octomap::OcTreeKey endKey;
        if(m_octree->coordToKeyChecked(point, endKey))
        {
              updateMinKey(endKey, m_updateBBXMin);
              updateMaxKey(endKey, m_updateBBXMax);
         }
        else
        {
              printf("could not generator key for endpoint");
        }
     }

      // all other points : free on ray, occupied on endpoings:
     for(auto p:nonground.points)
     {
         octomap::point3d point(p.x, p.y, p.z);
         //free cell
         if(m_octree->computeRayKeys(sensorOrigin, point, m_keyRay))
         {
            // free_cells.insert(m_keyRay.begin(),m_keyRay.end());
         }
         //occupided endpoint
         octomap::OcTreeKey key;
         if(m_octree->coordToKeyChecked(point, key))
         {
             occupied_cells.insert(key);
             updateMinKey(key, m_updateBBXMin);
             updateMaxKey(key, m_updateBBXMax);
             m_octree->averageNodeColor(p.x, p.y, p.z, p.r,p.g, p.b);
         }

     }pcl::PointCloud<pcl::PointXYZRGB>observation;
     for(octomap::KeySet::iterator it = free_cells.begin(), end= free_cells.end(); it!=end; ++it)
     {
         if(occupied_cells.find(*it) == occupied_cells.end())
         {
             m_octree->updateNode(*it, false);
         }
     }

     for(octomap::KeySet::iterator it = occupied_cells.begin(), end= occupied_cells.end(); it!=end; ++it)
     {
         m_octree->updateNode(*it, true);
     }

     m_octree->prune();
}

void MapDrawer::heightMapColor(double h, double& r, double &g, double& b)
{

    double s = 1.0;
    double v = 1.0;

    h -= floor(h);
    h *= 6;

    int i;
    double m, n, f;

    i = floor(h);
    f = h - i;

    if(!(i & 1))
    {
        f = 1 - f;
    }
    m = v * (1-s);
    n = v * (1- s*f);

    switch(i)
    {
        case 6:
        case 0:
            r = v; g = n; b = m;
            break;
        case 1:
            r = n; g = v; b = m;
            break;
        case 2:
            r = m; g = v; b = n;
            break;
        case 3:
            r = m; g = n; b = v;
            break;
        case 4:
            r = n; g = m; b = v;
            break;
        case 5:
            r = v; g = m; b = n;
            break;
        default:
            r = 1; g = 0.5; b = 0.5;
         break;

    }

}

void MapDrawer::SaveOctoMap(const char *filename)
{
    std::ofstream outfile(filename, std::ios_base::out | std::ios_base::binary);
    if (outfile.is_open())
    {
        m_octree->write(outfile);
        outfile.close();
    }
}

void MapDrawer::DrawOctoMap()
{
    vector<KeyFrame*> vKFs= mpMap->GetAllKeyFrames();
    int N = vKFs.size();

    if(N==0)
    {
        m_octree->clear();
        lastKeyframeSize = 0;
        return;
    }
    if(bIsLocalization == false)
    UpdateOctomap(vKFs);



    octomap::ColorOcTree::tree_iterator it  = m_octree->begin_tree();
    octomap::ColorOcTree::tree_iterator end = m_octree->end_tree();
    int counter = 0;
    double occ_thresh = 0.9;
    int level = 16;
    glClearColor(1.0f,1.0f,1.0f,1.0f);

    glDisable(GL_LIGHTING);
    glEnable (GL_BLEND);
    ////DRAW OCTOMAP BEGIN//////
    double stretch_factor = 128/(1 - occ_thresh); //occupancy range in which the displayed cubes can be
    for(; it != end; ++counter, ++it)
    {
        if(level != it.getDepth())
        {
            continue;
        }
        double occ = it->getOccupancy();
        if(occ < occ_thresh)
        {
            continue;
        }

        double minX, minY, minZ, maxX, maxY, maxZ;
        m_octree->getMetricMin(minX, minY, minZ);
        m_octree->getMetricMax(maxX, maxY, maxZ);

       float halfsize = it.getSize()/2.0;
       float x = it.getX();
       float y = it.getY();
       float z = it.getZ();
       double h = ( std::min(std::max((y-minY)/(maxY-minY), 0.0), 1.0))*0.8;
       double r, g, b;
       heightMapColor(h, r,g,b);
       glBegin(GL_TRIANGLES);
       //Front
       glColor3d(r, g, b);
       glVertex3f(x-halfsize,y-halfsize,z-halfsize);// - - - 1
       glVertex3f(x-halfsize,y+halfsize,z-halfsize);// - + - 2
       glVertex3f(x+halfsize,y+halfsize,z-halfsize);// + + -3

       glVertex3f(x-halfsize,y-halfsize,z-halfsize); // - - -
       glVertex3f(x+halfsize,y+halfsize,z-halfsize); // + + -
       glVertex3f(x+halfsize,y-halfsize,z-halfsize); // + - -4

       //Back
       glVertex3f(x-halfsize,y-halfsize,z+halfsize); // - - + 1
       glVertex3f(x+halfsize,y-halfsize,z+halfsize); // + - + 2
       glVertex3f(x+halfsize,y+halfsize,z+halfsize); // + + + 3

       glVertex3f(x-halfsize,y-halfsize,z+halfsize); // - - +
       glVertex3f(x+halfsize,y+halfsize,z+halfsize); // + + +
       glVertex3f(x-halfsize,y+halfsize,z+halfsize); // - + + 4

       //Left
       glVertex3f(x-halfsize,y-halfsize,z-halfsize); // - - - 1
       glVertex3f(x-halfsize,y-halfsize,z+halfsize); // - - + 2
       glVertex3f(x-halfsize,y+halfsize,z+halfsize); // - + + 3

       glVertex3f(x-halfsize,y-halfsize,z-halfsize); // - - -
       glVertex3f(x-halfsize,y+halfsize,z+halfsize); // - + +
       glVertex3f(x-halfsize,y+halfsize,z-halfsize); // - + - 4

       //Right
       glVertex3f(x+halfsize,y-halfsize,z-halfsize);
       glVertex3f(x+halfsize,y+halfsize,z-halfsize);
       glVertex3f(x+halfsize,y+halfsize,z+halfsize);

       glVertex3f(x+halfsize,y-halfsize,z-halfsize);
       glVertex3f(x+halfsize,y+halfsize,z+halfsize);
       glVertex3f(x+halfsize,y-halfsize,z+halfsize);

       //top
       glVertex3f(x-halfsize,y-halfsize,z-halfsize);
       glVertex3f(x+halfsize,y-halfsize,z-halfsize);
       glVertex3f(x+halfsize,y-halfsize,z+halfsize);

       glVertex3f(x-halfsize,y-halfsize,z-halfsize);
       glVertex3f(x+halfsize,y-halfsize,z+halfsize);
       glVertex3f(x-halfsize,y-halfsize,z+halfsize);

       //bottom
       glVertex3f(x-halfsize,y+halfsize,z-halfsize);
       glVertex3f(x-halfsize,y+halfsize,z+halfsize);
       glVertex3f(x+halfsize,y+halfsize,z+halfsize);

       glVertex3f(x-halfsize,y+halfsize,z-halfsize);
       glVertex3f(x+halfsize,y+halfsize,z+halfsize);
       glVertex3f(x+halfsize,y+halfsize,z-halfsize);
       glEnd();

       glBegin(GL_LINES);
       glColor3f(0,0,0);
       //
       glVertex3f(x-halfsize,y-halfsize,z-halfsize);// - - - 1
       glVertex3f(x-halfsize,y+halfsize,z-halfsize);

       glVertex3f(x-halfsize,y+halfsize,z-halfsize);// - + - 2
       glVertex3f(x+halfsize,y+halfsize,z-halfsize);// + + -3

       glVertex3f(x+halfsize,y+halfsize,z-halfsize);// + + -3
       glVertex3f(x+halfsize,y-halfsize,z-halfsize); // + - -4

       glVertex3f(x+halfsize,y-halfsize,z-halfsize); // + - -4
       glVertex3f(x-halfsize,y-halfsize,z-halfsize);// - - - 1


       // back

       glVertex3f(x-halfsize,y-halfsize,z+halfsize); // - - + 1
       glVertex3f(x+halfsize,y-halfsize,z+halfsize); // + - + 2

       glVertex3f(x+halfsize,y-halfsize,z+halfsize); // + - + 2
       glVertex3f(x+halfsize,y+halfsize,z+halfsize); // + + + 3

       glVertex3f(x+halfsize,y+halfsize,z+halfsize); // + + + 3
       glVertex3f(x-halfsize,y+halfsize,z+halfsize); // - + + 4

       glVertex3f(x-halfsize,y+halfsize,z+halfsize); // - + + 4
       glVertex3f(x-halfsize,y-halfsize,z+halfsize); // - - + 1

       // top
       glVertex3f(x+halfsize,y-halfsize,z-halfsize);
       glVertex3f(x+halfsize,y-halfsize,z+halfsize);

       glVertex3f(x-halfsize,y-halfsize,z+halfsize);
       glVertex3f(x-halfsize,y-halfsize,z-halfsize);

        // bottom

       glVertex3f(x-halfsize,y+halfsize,z+halfsize);
       glVertex3f(x+halfsize,y+halfsize,z+halfsize);

       glVertex3f(x-halfsize,y+halfsize,z-halfsize);
       glVertex3f(x+halfsize,y+halfsize,z-halfsize);
       glEnd();
    }
}

void MapDrawer::DrawGrid()
{
    glBegin(GL_LINES);
    glLineWidth(1);

    glColor3f(0.5,0.5,0.5); //gray
    int size =10;
    for(int i = -size; i <= size ; i++){

      glVertex3f(i,0.6,  size);
      glVertex3f(i, 0.6, -size);
      glVertex3f( size, 0.6, i);
      glVertex3f(-size, 0.6, i);
    }

    glEnd();
}
void MapDrawer::DrawObs(pcl::PointCloud<pcl::PointXYZRGB> observation)
{
    glPointSize(mPointSize);
    glBegin(GL_POINTS);
    glColor3f(1.0,0.0,0.0);

    for(int i=0; i<observation.points.size(); i++)
    {


        glVertex3f(observation.points[i].x,observation.points[i].y,observation.points[i].z);

    }

    glEnd();
}

void MapDrawer::Register(Map* pMap) {
  mpMap = pMap;
}
//Orbslam3
bool MapDrawer::ParseViewerParamFile(cv::FileStorage &fSettings)
{
    bool b_miss_params = false;

    cv::FileNode node = fSettings["Viewer.KeyFrameSize"];
    if(!node.empty())
    {
        mKeyFrameSize = node.real();
    }
    else
    {
        std::cerr << "*Viewer.KeyFrameSize parameter doesn't exist or is not a real number*" << std::endl;
        b_miss_params = true;
    }

    node = fSettings["Viewer.KeyFrameLineWidth"];
    if(!node.empty())
    {
        mKeyFrameLineWidth = node.real();
    }
    else
    {
        std::cerr << "*Viewer.KeyFrameLineWidth parameter doesn't exist or is not a real number*" << std::endl;
        b_miss_params = true;
    }

    node = fSettings["Viewer.GraphLineWidth"];
    if(!node.empty())
    {
        mGraphLineWidth = node.real();
    }
    else
    {
        std::cerr << "*Viewer.GraphLineWidth parameter doesn't exist or is not a real number*" << std::endl;
        b_miss_params = true;
    }

    node = fSettings["Viewer.PointSize"];
    if(!node.empty())
    {
        mPointSize = node.real();
    }
    else
    {
        std::cerr << "*Viewer.PointSize parameter doesn't exist or is not a real number*" << std::endl;
        b_miss_params = true;
    }

    node = fSettings["Viewer.CameraSize"];
    if(!node.empty())
    {
        mCameraSize = node.real();
    }
    else
    {
        std::cerr << "*Viewer.CameraSize parameter doesn't exist or is not a real number*" << std::endl;
        b_miss_params = true;
    }

    node = fSettings["Viewer.CameraLineWidth"];
    if(!node.empty())
    {
        mCameraLineWidth = node.real();
    }
    else
    {
        std::cerr << "*Viewer.CameraLineWidth parameter doesn't exist or is not a real number*" << std::endl;
        b_miss_params = true;
    }

    return !b_miss_params;
}

void MapDrawer::DrawMapPoints()
{
    Map* pActiveMap = mpAtlas->GetCurrentMap();
    if(!pActiveMap)
        return;

    const vector<MapPoint*> &vpMPs = pActiveMap->GetAllMapPoints();
    const vector<MapPoint*> &vpRefMPs = pActiveMap->GetReferenceMapPoints();

    set<MapPoint*> spRefMPs(vpRefMPs.begin(), vpRefMPs.end());

    if(vpMPs.empty())
        return;

    glPointSize(mPointSize);
    glBegin(GL_POINTS);
    glColor3f(0.0,0.0,0.0);

    for(size_t i=0, iend=vpMPs.size(); i<iend;i++)
    {
        if(vpMPs[i]->isBad() || spRefMPs.count(vpMPs[i]))
            continue;
        Eigen::Matrix<float,3,1> pos = vpMPs[i]->GetWorldPos();
        glVertex3f(pos(0),pos(1),pos(2));
    }
    glEnd();

    glPointSize(mPointSize);
    glBegin(GL_POINTS);
    glColor3f(1.0,0.0,0.0);

    for(set<MapPoint*>::iterator sit=spRefMPs.begin(), send=spRefMPs.end(); sit!=send; sit++)
    {
        if((*sit)->isBad())
            continue;
        Eigen::Matrix<float,3,1> pos = (*sit)->GetWorldPos();
        glVertex3f(pos(0),pos(1),pos(2));

    }

    glEnd();
}

void MapDrawer::DrawKeyFrames(const bool bDrawKF, const bool bDrawGraph, const bool bDrawInertialGraph, const bool bDrawOptLba)
{
    const float &w = mKeyFrameSize;
    const float h = w*0.75;
    const float z = w*0.6;

    Map* pActiveMap = mpAtlas->GetCurrentMap();
    // DEBUG LBA
    std::set<long unsigned int> sOptKFs = pActiveMap->msOptKFs;
    std::set<long unsigned int> sFixedKFs = pActiveMap->msFixedKFs;

    if(!pActiveMap)
        return;

    const vector<KeyFrame*> vpKFs = pActiveMap->GetAllKeyFrames();

    if(bDrawKF)
    {
        for(size_t i=0; i<vpKFs.size(); i++)
        {
            KeyFrame* pKF = vpKFs[i];
            Eigen::Matrix4f Twc = pKF->GetPoseInverse().matrix();
            unsigned int index_color = pKF->mnOriginMapId;

            glPushMatrix();

            glMultMatrixf((GLfloat*)Twc.data());

            if(!pKF->GetParent()) // It is the first KF in the map
            {
                glLineWidth(mKeyFrameLineWidth*5);
                glColor3f(1.0f,0.0f,0.0f);
                glBegin(GL_LINES);
            }
            else
            {
                //cout << "Child KF: " << vpKFs[i]->mnId << endl;
                glLineWidth(mKeyFrameLineWidth);
                if (bDrawOptLba) {
                    if(sOptKFs.find(pKF->mnId) != sOptKFs.end())
                    {
                        glColor3f(0.0f,1.0f,0.0f); // Green -> Opt KFs
                    }
                    else if(sFixedKFs.find(pKF->mnId) != sFixedKFs.end())
                    {
                        glColor3f(1.0f,0.0f,0.0f); // Red -> Fixed KFs
                    }
                    else
                    {
                        glColor3f(0.0f,0.0f,1.0f); // Basic color
                    }
                }
                else
                {
                    glColor3f(0.0f,0.0f,1.0f); // Basic color
                }
                glBegin(GL_LINES);
            }

            glVertex3f(0,0,0);
            glVertex3f(w,h,z);
            glVertex3f(0,0,0);
            glVertex3f(w,-h,z);
            glVertex3f(0,0,0);
            glVertex3f(-w,-h,z);
            glVertex3f(0,0,0);
            glVertex3f(-w,h,z);

            glVertex3f(w,h,z);
            glVertex3f(w,-h,z);

            glVertex3f(-w,h,z);
            glVertex3f(-w,-h,z);

            glVertex3f(-w,h,z);
            glVertex3f(w,h,z);

            glVertex3f(-w,-h,z);
            glVertex3f(w,-h,z);
            glEnd();

            glPopMatrix();

            glEnd();
        }
    }

    if(bDrawGraph)
    {
        glLineWidth(mGraphLineWidth);
        glColor4f(0.0f,1.0f,0.0f,0.6f);
        glBegin(GL_LINES);

        // cout << "-----------------Draw graph-----------------" << endl;
        for(size_t i=0; i<vpKFs.size(); i++)
        {
            // Covisibility Graph
            const vector<KeyFrame*> vCovKFs = vpKFs[i]->GetCovisiblesByWeight(100);
            Eigen::Vector3f Ow = vpKFs[i]->GetCameraCenter();
            if(!vCovKFs.empty())
            {
                for(vector<KeyFrame*>::const_iterator vit=vCovKFs.begin(), vend=vCovKFs.end(); vit!=vend; vit++)
                {
                    if((*vit)->mnId<vpKFs[i]->mnId)
                        continue;
                    Eigen::Vector3f Ow2 = (*vit)->GetCameraCenter();
                    glVertex3f(Ow(0),Ow(1),Ow(2));
                    glVertex3f(Ow2(0),Ow2(1),Ow2(2));
                }
            }

            // Spanning tree
            KeyFrame* pParent = vpKFs[i]->GetParent();
            if(pParent)
            {
                Eigen::Vector3f Owp = pParent->GetCameraCenter();
                glVertex3f(Ow(0),Ow(1),Ow(2));
                glVertex3f(Owp(0),Owp(1),Owp(2));
            }

            // Loops
            set<KeyFrame*> sLoopKFs = vpKFs[i]->GetLoopEdges();
            for(set<KeyFrame*>::iterator sit=sLoopKFs.begin(), send=sLoopKFs.end(); sit!=send; sit++)
            {
                if((*sit)->mnId<vpKFs[i]->mnId)
                    continue;
                Eigen::Vector3f Owl = (*sit)->GetCameraCenter();
                glVertex3f(Ow(0),Ow(1),Ow(2));
                glVertex3f(Owl(0),Owl(1),Owl(2));
            }
        }

        glEnd();
    }

    if(bDrawInertialGraph && pActiveMap->isImuInitialized())
    {
        glLineWidth(mGraphLineWidth);
        glColor4f(1.0f,0.0f,0.0f,0.6f);
        glBegin(GL_LINES);

        //Draw inertial links
        for(size_t i=0; i<vpKFs.size(); i++)
        {
            KeyFrame* pKFi = vpKFs[i];
            Eigen::Vector3f Ow = pKFi->GetCameraCenter();
            KeyFrame* pNext = pKFi->mNextKF;
            if(pNext)
            {
                Eigen::Vector3f Owp = pNext->GetCameraCenter();
                glVertex3f(Ow(0),Ow(1),Ow(2));
                glVertex3f(Owp(0),Owp(1),Owp(2));
            }
        }

        glEnd();
    }

    vector<Map*> vpMaps = mpAtlas->GetAllMaps();

    if(bDrawKF)
    {
        for(Map* pMap : vpMaps)
        {
            if(pMap == pActiveMap)
                continue;

            vector<KeyFrame*> vpKFs = pMap->GetAllKeyFrames();

            for(size_t i=0; i<vpKFs.size(); i++)
            {
                KeyFrame* pKF = vpKFs[i];
                Eigen::Matrix4f Twc = pKF->GetPoseInverse().matrix();
                unsigned int index_color = pKF->mnOriginMapId;

                glPushMatrix();

                glMultMatrixf((GLfloat*)Twc.data());

                if(!vpKFs[i]->GetParent()) // It is the first KF in the map
                {
                    glLineWidth(mKeyFrameLineWidth*5);
                    glColor3f(1.0f,0.0f,0.0f);
                    glBegin(GL_LINES);
                }
                else
                {
                    glLineWidth(mKeyFrameLineWidth);
                    glColor3f(mfFrameColors[index_color][0],mfFrameColors[index_color][1],mfFrameColors[index_color][2]);
                    glBegin(GL_LINES);
                }

                glVertex3f(0,0,0);
                glVertex3f(w,h,z);
                glVertex3f(0,0,0);
                glVertex3f(w,-h,z);
                glVertex3f(0,0,0);
                glVertex3f(-w,-h,z);
                glVertex3f(0,0,0);
                glVertex3f(-w,h,z);

                glVertex3f(w,h,z);
                glVertex3f(w,-h,z);

                glVertex3f(-w,h,z);
                glVertex3f(-w,-h,z);

                glVertex3f(-w,h,z);
                glVertex3f(w,h,z);

                glVertex3f(-w,-h,z);
                glVertex3f(w,-h,z);
                glEnd();

                glPopMatrix();
            }
        }
    }
}

void MapDrawer::DrawCurrentCamera(pangolin::OpenGlMatrix &Twc)
{
    const float &w = mCameraSize;
    const float h = w*0.75;
    const float z = w*0.6;

    glPushMatrix();

#ifdef HAVE_GLES
        glMultMatrixf(Twc.m);
#else
        glMultMatrixd(Twc.m);
#endif

    glLineWidth(mCameraLineWidth);
    glColor3f(0.0f,1.0f,0.0f);
    glBegin(GL_LINES);
    glVertex3f(0,0,0);
    glVertex3f(w,h,z);
    glVertex3f(0,0,0);
    glVertex3f(w,-h,z);
    glVertex3f(0,0,0);
    glVertex3f(-w,-h,z);
    glVertex3f(0,0,0);
    glVertex3f(-w,h,z);

    glVertex3f(w,h,z);
    glVertex3f(w,-h,z);

    glVertex3f(-w,h,z);
    glVertex3f(-w,-h,z);

    glVertex3f(-w,h,z);
    glVertex3f(w,h,z);

    glVertex3f(-w,-h,z);
    glVertex3f(w,-h,z);
    glEnd();

    glPopMatrix();
}


void MapDrawer::SetCurrentCameraPose(const Sophus::SE3f &Tcw)
{
    unique_lock<mutex> lock(mMutexCamera);
    mCameraPose = Tcw.inverse();
}

void MapDrawer::GetCurrentOpenGLCameraMatrix(pangolin::OpenGlMatrix &M, pangolin::OpenGlMatrix &MOw)
{
    Eigen::Matrix4f Twc;
    {
        unique_lock<mutex> lock(mMutexCamera);
        Twc = mCameraPose.matrix();
    }

    for (int i = 0; i<4; i++) {
        M.m[4*i] = Twc(0,i);
        M.m[4*i+1] = Twc(1,i);
        M.m[4*i+2] = Twc(2,i);
        M.m[4*i+3] = Twc(3,i);
    }

    MOw.SetIdentity();
    MOw.m[12] = Twc(0,3);
    MOw.m[13] = Twc(1,3);
    MOw.m[14] = Twc(2,3);
}
} //namespace ORB_SLAM
