/* Copyright (c) 2013-2015 Stanislaw Halik <sthalik@misaki.pl>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 */

#include "cv/video-widget.hpp"
#include "ftnoir_tracker_aruco.h"
#include "cv/video-property-page.hpp"
#include "compat/camera-names.hpp"
#include "compat/sleep.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/calib3d.hpp>

#include <QMutexLocker>
#include <QDebug>

#include <vector>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <iterator>

struct resolution_tuple
{
    int width;
    int height;
};

static constexpr const resolution_tuple resolution_choices[] =
{
    { 640, 480 },
    { 320, 240 },
    { 0, 0 }
};

constexpr const double aruco_tracker::RC;
constexpr const double aruco_tracker::size_min;
constexpr const double aruco_tracker::size_max;

aruco_tracker::aruco_tracker() :
    stop(false),
    layout(nullptr),
    videoWidget(nullptr),
    fps(0),
    obj_points(4),
    intrinsics(cv::Matx33d::eye()),
    rmat(cv::Matx33d::eye()),
    roi_points(4),
    last_roi(65535, 65535, 0, 0)
{
    // param 2 ignored for Otsu thresholding. it's required to use our fork of Aruco.
    detector.setThresholdParams(5, -1);
    detector.setDesiredSpeed(3);
    detector._thresMethod = aruco::MarkerDetector::FIXED_THRES;
}

aruco_tracker::~aruco_tracker()
{
    stop = true;
    wait();
    if (videoWidget)
        delete videoWidget;
    if(layout)
        delete layout;
    // fast start/stop causes breakage
    portable::sleep(1000);
    camera.release();
}

void aruco_tracker::start_tracker(QFrame* videoframe)
{
    videoframe->show();
    videoWidget = new cv_video_widget(videoframe);
    QHBoxLayout* layout_ = new QHBoxLayout();
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->addWidget(videoWidget);
    if (videoframe->layout())
        delete videoframe->layout();
    videoframe->setLayout(layout_);
    videoWidget->show();
    start();
    for (int i = 0; i < 6; i++)
        pose[i] = 0;
    layout = layout_;
}

void aruco_tracker::getRT(cv::Matx33d& r_, cv::Vec3d& t_)
{
    QMutexLocker l(&mtx);

    r_ = r;
    t_ = t;
}

bool aruco_tracker::detect_with_roi()
{
    if (last_roi.width > 1 && last_roi.height > 1)
    {
        detector.setMinMaxSize(std::min(1., std::max(.01, size_min * grayscale.cols / last_roi.width)),
                               std::max(.01, std::min(1., size_max * grayscale.cols / last_roi.width)));

        detector.detect(grayscale(last_roi), markers, cv::Mat(), cv::Mat(), -1, false);

        if (markers.size() == 1 && markers[0].size() == 4)
        {
            auto& m = markers[0];
            for (unsigned i = 0; i < 4; i++)
            {
                auto& p = m[i];
                p.x += last_roi.x;
                p.y += last_roi.y;
            }
            return true;
        }
    }

    last_roi = cv::Rect(65535, 65535, 0, 0);
    return false;
}

bool aruco_tracker::detect_without_roi()
{
    detector.setMinMaxSize(size_min, size_max);
    detector.detect(grayscale, markers, cv::Mat(), cv::Mat(), -1, false);
    return markers.size() == 1 && markers[0].size() == 4;
}

bool aruco_tracker::open_camera()
{
    int rint = s.resolution;
    if (rint < 0 || rint >= (int)(sizeof(resolution_choices) / sizeof(resolution_tuple)))
        rint = 0;
    resolution_tuple res = resolution_choices[rint];
    int fps;
    switch (static_cast<int>(s.force_fps))
    {
    default:
    case 0:
        fps = 0;
        break;
    case 1:
        fps = 30;
        break;
    case 2:
        fps = 60;
        break;
    case 3:
        fps = 75;
        break;
    case 4:
        fps = 125;
        break;
    case 5:
        fps = 200;
        break;
    }

    QMutexLocker l(&camera_mtx);

    camera = cv::VideoCapture(camera_name_to_index(s.camera_name));
    if (res.width)
    {
        camera.set(cv::CAP_PROP_FRAME_WIDTH, res.width);
        camera.set(cv::CAP_PROP_FRAME_HEIGHT, res.height);
    }
    if (fps)
        camera.set(cv::CAP_PROP_FPS, fps);

    if (!camera.isOpened())
    {
        qDebug() << "aruco tracker: can't open camera";
        return false;
    }
    return true;
}

void aruco_tracker::set_intrinsics()
{
    static constexpr double pi = 3.1415926f;
    const int w = grayscale.cols, h = grayscale.rows;
    const double diag_fov = static_cast<int>(s.fov) * pi / 180.;
    const double fov_w = 2.*atan(tan(diag_fov/2.)/sqrt(1. + h/(double)w * h/(double)w));
    const double fov_h = 2.*atan(tan(diag_fov/2.)/sqrt(1. + w/(double)h * w/(double)h));
    const double focal_length_w = .5 * w / tan(.5 * fov_w);
    const double focal_length_h = .5 * h / tan(.5 * fov_h);

    intrinsics(0, 0) = focal_length_w;
    intrinsics(0, 2) = grayscale.cols/2;
    intrinsics(1, 1) = focal_length_h;
    intrinsics(1, 2) = grayscale.rows/2;
}

void aruco_tracker::update_fps()
{
    const double dt = fps_timer.elapsed_seconds();
    fps_timer.start();
    const double alpha = dt/(dt + RC);

    if (dt > 1e-3)
    {
        fps *= 1 - alpha;
        fps += alpha * (1./dt + .8);
    }
}

void aruco_tracker::draw_ar(bool ok)
{
    if (ok)
    {
        const auto& m = markers[0];
        for (unsigned i = 0; i < 4; i++)
            cv::line(frame, m[i], m[(i+1)%4], cv::Scalar(0, 0, 255), 2, 8);
    }

    char buf[9];
    ::snprintf(buf, sizeof(buf)-1, "Hz: %d", clamp(int(fps), 0, 9999));
    buf[sizeof(buf)-1] = '\0';
    cv::putText(frame, buf, cv::Point(10, 32), cv::FONT_HERSHEY_PLAIN, 2, cv::Scalar(0, 255, 0), 1);
}

void aruco_tracker::clamp_last_roi()
{
    if (last_roi.x < 0)
        last_roi.x = 0;
    if (last_roi.y < 0)
        last_roi.y = 0;
    if (last_roi.width < 1)
        last_roi.width = 1;
    if (last_roi.height < 1)
        last_roi.height = 1;
    if (last_roi.x >= color.cols-1)
        last_roi.x = color.cols-1;
    if (last_roi.width >= color.cols-1)
        last_roi.width = color.cols-1;
    if (last_roi.y >= color.rows-1)
        last_roi.y = color.rows-1;
    if (last_roi.height >= color.rows-1)
        last_roi.height = color.rows-1;

    last_roi.width -= last_roi.x;
    last_roi.height -= last_roi.y;
}

void aruco_tracker::set_points()
{
    using f = float;
    const f hx = f(s.headpos_x), hy = f(s.headpos_y), hz = f(s.headpos_z);

    static constexpr float size = 40;

    const int x1=1, x2=2, x3=3, x4=0;

    obj_points[x1] = cv::Point3f(-size + hx, -size + hy, 0 + hz);
    obj_points[x2] = cv::Point3f(size + hx, -size + hy, 0 + hz);
    obj_points[x3] = cv::Point3f(size + hx, size + hy, 0 + hz);
    obj_points[x4] = cv::Point3f(-size + hx, size + hy, 0 + hz);
}

void aruco_tracker::draw_centroid()
{
    repr2.clear();

    static const std::vector<cv::Point3f> centroid { cv::Point3f(0, 0, 0) };

    cv::projectPoints(centroid, rvec, tvec, intrinsics, cv::noArray(), repr2);

    cv::circle(frame, repr2[0], 4, cv::Scalar(255, 0, 255), -1);
}

void aruco_tracker::set_last_roi()
{
    roi_projection.clear();

    using f = float;
    cv::Point3f h(f(s.headpos_x), f(s.headpos_y), f(s.headpos_z));
    for (unsigned i = 0; i < 4; i++)
    {
        cv::Point3f pt(obj_points[i] - h);
        roi_points[i] = pt * c_search_window;
    }

    cv::projectPoints(roi_points, rvec, tvec, intrinsics, cv::noArray(), roi_projection);

    set_roi_from_projection();
}

void aruco_tracker::set_rmat()
{
    cv::Rodrigues(rvec, rmat);

    euler = cv::RQDecomp3x3(rmat, m_r, m_q);

    QMutexLocker lck(&mtx);

    for (int i = 0; i < 3; i++)
        pose[i] = tvec(i) * .1;

    pose[Yaw] = euler[1];
    pose[Pitch] = -euler[0];
    pose[Roll] = euler[2];

    r = rmat;
    t = cv::Vec3d(tvec[0], -tvec[1], tvec[2]);
}

void aruco_tracker::set_roi_from_projection()
{
    last_roi = cv::Rect(color.cols-1, color.rows-1, 0, 0);

    for (unsigned i = 0; i < 4; i++)
    {
        const auto& proj = roi_projection[i];
        int min_x = std::min(int(proj.x), last_roi.x),
            min_y = std::min(int(proj.y), last_roi.y);

        int max_x = std::max(int(proj.x), last_roi.width),
            max_y = std::max(int(proj.y), last_roi.height);

        last_roi.x = min_x;
        last_roi.y = min_y;

        last_roi.width = max_x;
        last_roi.height = max_y;
    }

    clamp_last_roi();
}

void aruco_tracker::run()
{
    cv::setNumThreads(0);

    using std::fabs;
    using std::atan;
    using std::tan;
    using std::sqrt;

    if (!open_camera())
        return;

    fps_timer.start();

    while (!stop)
    {
        {
            QMutexLocker l(&camera_mtx);

            if (!camera.read(color))
                continue;
        }

        cv::cvtColor(color, grayscale, cv::COLOR_RGB2GRAY);

        color.copyTo(frame);

        set_intrinsics();

        update_fps();

        markers.clear();

        const bool ok = detect_with_roi() || detect_without_roi();

        if (ok)
        {
            set_points();

            if (!cv::solvePnP(obj_points, markers[0], intrinsics, cv::noArray(), rvec, tvec, false, cv::SOLVEPNP_ITERATIVE))
                goto fail;

            set_last_roi();
            draw_centroid();
            set_rmat();
        }
        else
fail:
            // no marker found, reset search region
            last_roi = cv::Rect(65535, 65535, 0, 0);

        draw_ar(ok);

        if (frame.rows > 0)
            videoWidget->update_image(frame);
    }
}

void aruco_tracker::data(double *data)
{
    QMutexLocker lck(&mtx);

    data[Yaw] = pose[Yaw];
    data[Pitch] = pose[Pitch];
    data[Roll] = pose[Roll];
    data[TX] = pose[TX];
    data[TY] = pose[TY];
    data[TZ] = pose[TZ];
}

aruco_dialog::aruco_dialog()
{
    tracker = nullptr;
    calib_timer.setInterval(250);
    ui.setupUi(this);
    setAttribute(Qt::WA_NativeWindow, true);
    ui.cameraName->addItems(get_camera_names());
    tie_setting(s.camera_name, ui.cameraName);
    tie_setting(s.resolution, ui.resolution);
    tie_setting(s.force_fps, ui.cameraFPS);
    tie_setting(s.fov, ui.cameraFOV);
    tie_setting(s.headpos_x, ui.cx);
    tie_setting(s.headpos_y, ui.cy);
    tie_setting(s.headpos_z, ui.cz);
    connect(ui.buttonBox, SIGNAL(accepted()), this, SLOT(doOK()));
    connect(ui.buttonBox, SIGNAL(rejected()), this, SLOT(doCancel()));
    connect(ui.btn_calibrate, SIGNAL(clicked()), this, SLOT(toggleCalibrate()));
    connect(this, SIGNAL(destroyed()), this, SLOT(cleanupCalib()));
    connect(&calib_timer, SIGNAL(timeout()), this, SLOT(update_tracker_calibration()));
    connect(ui.camera_settings, SIGNAL(clicked()), this, SLOT(camera_settings()));

    connect(&s.camera_name, SIGNAL(valueChanged(const QString&)), this, SLOT(update_camera_settings_state(const QString&)));

    update_camera_settings_state(s.camera_name);
}

void aruco_dialog::toggleCalibrate()
{
    if (!calib_timer.isActive())
    {
        s.headpos_x = 0;
        s.headpos_y = 0;
        s.headpos_z = 0;
        calibrator.reset();
        calib_timer.start();
    }
    else
    {
        cleanupCalib();

        auto pos = calibrator.get_estimate();
        s.headpos_x = pos(0);
        s.headpos_y = pos(1);
        s.headpos_z = pos(2);
    }
}

void aruco_dialog::cleanupCalib()
{
    if (calib_timer.isActive())
        calib_timer.stop();
}

void aruco_dialog::update_tracker_calibration()
{
    if (calib_timer.isActive() && tracker)
    {
        cv::Matx33d r;
        cv::Vec3d t;
        tracker->getRT(r, t);
        calibrator.update(r, t);
    }
}

void aruco_dialog::doOK()
{
    s.b->save();
    close();
}

void aruco_dialog::doCancel()
{
    close();
}

void aruco_dialog::camera_settings()
{
    if (tracker)
    {
        QMutexLocker l(&tracker->camera_mtx);
        video_property_page::show_from_capture(tracker->camera, camera_name_to_index(s.camera_name));
    }
    else
        video_property_page::show(camera_name_to_index(s.camera_name));
}

void aruco_dialog::update_camera_settings_state(const QString& name)
{
    ui.camera_settings->setEnabled(video_property_page::should_show_dialog(name));
}

OPENTRACK_DECLARE_TRACKER(aruco_tracker, aruco_dialog, aruco_metadata)
