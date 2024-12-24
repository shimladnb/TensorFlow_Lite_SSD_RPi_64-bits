#include <stdio.h>
#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/highgui.hpp>
#include <fstream>
#include <iostream>
#include <opencv2/core/ocl.hpp>
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/string_util.h"
#include "tensorflow/lite/model.h"
#include <cmath>

#include "libcamera/libcamera.h"
#include <cmath>

#include "osc/OscOutboundPacketStream.h"
#include "ip/UdpSocket.h"
#include <cstdlib>
#include <cstring>

#define ADDRESS "23.23.23.100"
#define PORT 7000
#define OUTPUT_BUFFER_SIZE 1024

using namespace cv;
using namespace std;

const size_t width = 300;
const size_t height = 300;
//width=1296,height=972

const float confidence_threshold = 0.45f;

std::vector<std::string> Labels;
std::unique_ptr<tflite::Interpreter> interpreter;

UdpTransmitSocket transmitSocket( IpEndpointName( ADDRESS, PORT ) );




static bool getFileContent(std::string fileName)
{

    // Open the File
    std::ifstream in(fileName.c_str());
    // Check if object is valid
    if(!in.is_open()) return false;

    std::string str;
    // Read the next line from File untill it reaches the end.
    while (std::getline(in, str))
    {
        // Line contains string of length > 0 then save it in vector
        if(str.size()>0) Labels.push_back(str);
    }
    // Close The File
    in.close();
    return true;
}

void SendOscMessage(const char* id,int index, float locx, float locy)
{
    char buffer[OUTPUT_BUFFER_SIZE];
    osc::OutboundPacketStream p( buffer, OUTPUT_BUFFER_SIZE );
    p << osc::BeginBundleImmediate;
    p << osc::BeginMessage( "/object/info" );
    p << id;
    p << index;
    p << locx;
    p << locy;
    p << osc::EndMessage;
    p << osc::EndBundle;
    transmitSocket.Send( p.Data(), p.Size() );
};


void detect_from_video(Mat &src)
{
    Mat image;
    int cam_width =src.cols;
    int cam_height=src.rows;

    // copy image to input as input tensor
    cv::resize(src, image, Size(width,height));
    memcpy(interpreter->typed_input_tensor<uchar>(0), image.data, image.total() * image.elemSize());

    interpreter->SetAllowFp16PrecisionForFp32(true);
    interpreter->SetNumThreads(4);      //quad core

//        cout << "tensors size: " << interpreter->tensors_size() << "\n";
//        cout << "nodes size: " << interpreter->nodes_size() << "\n";
//        cout << "inputs: " << interpreter->inputs().size() << "\n";
//        cout << "input(0) name: " << interpreter->GetInputName(0) << "\n";
//        cout << "outputs: " << interpreter->outputs().size() << "\n";

    interpreter->Invoke();      // run your model

    const float* detection_locations = interpreter->tensor(interpreter->outputs()[0])->data.f;
    const float* detection_classes=interpreter->tensor(interpreter->outputs()[1])->data.f;
    const float* detection_scores = interpreter->tensor(interpreter->outputs()[2])->data.f;
    const int    num_detections = *interpreter->tensor(interpreter->outputs()[3])->data.f;

    //there are ALWAYS 10 detections no matter how many objects are detectable
//        cout << "number of detections: " << num_detections << "\n";


    for(int i = 0; i < num_detections; i++)
    {
        if(detection_scores[i] > confidence_threshold)
        {
            int  det_index = (int)detection_classes[i]+1;
            float y1=detection_locations[4*i  ]*cam_height;
            float x1=detection_locations[4*i+1]*cam_width;
            float y2=detection_locations[4*i+2]*cam_height;
            float x2=detection_locations[4*i+3]*cam_width;

            float xcn= (detection_locations[4*i+1]+detection_locations[4*i+3])/2;
            float ycn= (detection_locations[4*i  ]+detection_locations[4*i+2])/2;

            float xc= (x1+x2)/2;
            float yc= (y1+y2)/2;

            //color = list(np.random.random(size=3) * 256);
            Rect rec((int)x1, (int)y1, (int)(x2 - x1), (int)(y2 - y1));
            rectangle(src,rec, Scalar(255, 0, 255), 1, 8, 0);
            circle(src, Point_(xc,yc), 5, Scalar(255, 0, 255), 1, FILLED, 0);
            putText(src, format("%s", Labels[det_index].c_str()), Point(x1, y1-5),FONT_HERSHEY_SIMPLEX,0.7f, Scalar(0, 0, 255), 1, 8, 0);

            SendOscMessage(Labels[det_index].c_str(),det_index, xcn, ycn);
        }
    }
}

int main(int argc,char ** argv)
{


    float f;
    float FPS[16];
    int i, Fcnt=0;
    Mat frame;
    const    char* gst = "libcamerasrc camera-name=/base/axi/pcie@120000/rp1/i2c@80000/ov5647@36 ! video/x-raw,width=1296,height=972,framerate=20/1,format=RGBx ! videoconvert ! videoscale !  video/x-raw,width=1296,height=972,format=BGR ! appsink";
    chrono::steady_clock::time_point Tbegin, Tend;

    for(i=0; i<16; i++) FPS[i]=0.0;

    // Load model
    std::unique_ptr<tflite::FlatBufferModel> model = tflite::FlatBufferModel::BuildFromFile("detect.tflite");

    // Build the interpreter
    tflite::ops::builtin::BuiltinOpResolver resolver;
    tflite::InterpreterBuilder(*model.get(), resolver)(&interpreter);

    interpreter->AllocateTensors();

    // Get the names
    bool result = getFileContent("COCO_labels.txt");
    if(!result)
    {
        cout << "loading labels failed";
        exit(-1);
    }

    VideoCapture cap(gst);
    if (!cap.isOpened())
    {
        cerr << "ERROR: Unable to open the camera" << endl;
        return 0;
    }

    cout << "Start grabbing, press ESC on Live window to terminate" << endl;
    while(1)
    {

//        frame=imread("Traffic.jpg");  //need to refresh frame before dnn class detection
        cap >> frame;
        if (frame.empty())
        {
            cerr << "ERROR: Unable to grab from the camera" << endl;
            break;
        }



        Tbegin = chrono::steady_clock::now();

        detect_from_video(frame);

        Tend = chrono::steady_clock::now();
        //calculate frame rate
        f = chrono::duration_cast <chrono::milliseconds> (Tend - Tbegin).count();
        if(f>0.0) FPS[((Fcnt++)&0x0F)]=1000.0/f;
        for(f=0.0, i=0; i<16; i++)
        {
            f+=FPS[i];
        }
        putText(frame, format("FPS %0.2f", f/16),Point(10,20),FONT_HERSHEY_SIMPLEX,0.6, Scalar(0, 0, 255));

        //show output
//        cout << "FPS" << f/16 << endl;
        imshow("RPi 4 - 1,9 GHz - 2 Mb RAM", frame);

        char esc = waitKey(5);
        if(esc == 27)
        break;
    }

    cout << "Closing the camera" << endl;
    destroyAllWindows();
    cout << "Bye!" << endl;

    return 0;
}
