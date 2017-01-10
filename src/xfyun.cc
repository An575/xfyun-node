// addon.cc
#include <node.h>
#include <nan.h>
#include <stdlib.h>
/*
* 语音识别（Automatic Speech Recognition）技术能够从语音中识别出特定的命令词或语句模式。
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "qisr.h"
#include "msp_cmn.h"
#include "msp_errors.h"

#define BUFFER_SIZE 4096
#define HINTS_SIZE 100
#define GRAMID_LEN 128
#define FRAME_LEN 640

namespace xfy
{

using v8::Function;
using v8::FunctionCallbackInfo;
using v8::Isolate;
using v8::Local;
using v8::Null;
using v8::Object;
using v8::String;
using v8::Value;

/**
   * 听写（iat）就是把任意语音转换成文字
   */
void run_iat(const char *audio_file, const char *session_begin_params)
{
  const char *session_id = NULL;
  char rec_result[BUFFER_SIZE] = {NULL};
  char hints[HINTS_SIZE] = {NULL}; //hints为结束本次会话的原因描述，由用户自定义
  unsigned int total_len = 0;
  int aud_stat = MSP_AUDIO_SAMPLE_CONTINUE; //音频状态
  int ep_stat = MSP_EP_LOOKING_FOR_SPEECH;  //端点检测
  int rec_stat = MSP_REC_STATUS_SUCCESS;    //识别状态
  int errcode = MSP_SUCCESS;

  FILE *f_pcm = NULL;
  char *p_pcm = NULL;
  long pcm_count = 0;
  long pcm_size = 0;
  long read_size = 0;

  if (NULL == audio_file)
    goto iat_exit;

  f_pcm = fopen(audio_file, "rb");
  if (NULL == f_pcm)
  {
    printf("\nopen [%s] failed! \n", audio_file);
    goto iat_exit;
  }

  fseek(f_pcm, 0, SEEK_END);
  pcm_size = ftell(f_pcm); //获取音频文件大小
  fseek(f_pcm, 0, SEEK_SET);

  p_pcm = (char *)malloc(pcm_size);
  if (NULL == p_pcm)
  {
    printf("\nout of memory! \n");
    goto iat_exit;
  }

  read_size = fread((void *)p_pcm, 1, pcm_size, f_pcm); //读取音频文件内容
  if (read_size != pcm_size)
  {
    printf("\nread [%s] error!\n", audio_file);
    goto iat_exit;
  }

  printf("\n开始语音听写 ...\n");
  session_id = QISRSessionBegin(NULL, session_begin_params, &errcode); //听写不需要语法，第一个参数为NULL
  if (MSP_SUCCESS != errcode)
  {
    printf("\nQISRSessionBegin failed! error code:%d\n", errcode);
    goto iat_exit;
  }

  while (1)
  {
    unsigned int len = 10 * FRAME_LEN; // 每次写入200ms音频(16k，16bit)：1帧音频20ms，10帧=200ms。16k采样率的16位音频，一帧的大小为640Byte
    int ret = 0;

    if (pcm_size < 2 * len)
      len = pcm_size;
    if (len <= 0)
      break;

    aud_stat = MSP_AUDIO_SAMPLE_CONTINUE;
    if (0 == pcm_count)
      aud_stat = MSP_AUDIO_SAMPLE_FIRST;

    printf(">");
    ret = QISRAudioWrite(session_id, (const void *)&p_pcm[pcm_count], len, aud_stat, &ep_stat, &rec_stat);
    if (MSP_SUCCESS != ret)
    {
      printf("\nQISRAudioWrite failed! error code:%d\n", ret);
      goto iat_exit;
    }

    pcm_count += (long)len;
    pcm_size -= (long)len;

    if (MSP_REC_STATUS_SUCCESS == rec_stat) //已经有部分听写结果
    {
      const char *rslt = QISRGetResult(session_id, &rec_stat, 0, &errcode);
      if (MSP_SUCCESS != errcode)
      {
        printf("\nQISRGetResult failed! error code: %d\n", errcode);
        goto iat_exit;
      }
      if (NULL != rslt)
      {
        unsigned int rslt_len = strlen(rslt);
        total_len += rslt_len;
        if (total_len >= BUFFER_SIZE)
        {
          printf("\nno enough buffer for rec_result !\n");
          goto iat_exit;
        }
        strncat(rec_result, rslt, rslt_len);
      }
    }

    if (MSP_EP_AFTER_SPEECH == ep_stat)
      break;
    usleep(200 * 1000); //模拟人说话时间间隙。200ms对应10帧的音频
  }
  errcode = QISRAudioWrite(session_id, NULL, 0, MSP_AUDIO_SAMPLE_LAST, &ep_stat, &rec_stat);
  if (MSP_SUCCESS != errcode)
  {
    printf("\nQISRAudioWrite failed! error code:%d \n", errcode);
    goto iat_exit;
  }

  while (MSP_REC_STATUS_COMPLETE != rec_stat)
  {
    const char *rslt = QISRGetResult(session_id, &rec_stat, 0, &errcode);
    if (MSP_SUCCESS != errcode)
    {
      printf("\nQISRGetResult failed, error code: %d\n", errcode);
      goto iat_exit;
    }
    if (NULL != rslt)
    {
      unsigned int rslt_len = strlen(rslt);
      total_len += rslt_len;
      if (total_len >= BUFFER_SIZE)
      {
        printf("\nno enough buffer for rec_result !\n");
        goto iat_exit;
      }
      strncat(rec_result, rslt, rslt_len);
    }
    usleep(150 * 1000); //防止频繁占用CPU
  }
  printf("\n语音听写结束\n");
  printf("=============================================================\n");
  printf("%s\n", rec_result);
  printf("=============================================================\n");

iat_exit:
  if (NULL != f_pcm)
  {
    fclose(f_pcm);
    f_pcm = NULL;
  }
  if (NULL != p_pcm)
  {
    free(p_pcm);
    p_pcm = NULL;
  }

  QISRSessionEnd(session_id, hints);
}

/**
   * 识别（asr）在特定的范围内把语音转化成文字（分为命令词识别和语法识别）
   */
void run_asr(const char *audio_file, const char *params, char *grammar_id)
{
  const char *session_id = NULL;
  char rec_result[BUFFER_SIZE] = {'\0'};
  char hints[HINTS_SIZE] = {'\0'}; //hints为结束本次会话的原因描述，由用户自定义
  unsigned int total_len = 0;
  int aud_stat = MSP_AUDIO_SAMPLE_CONTINUE; //音频状态
  int ep_stat = MSP_EP_LOOKING_FOR_SPEECH;  //端点检测
  int rec_stat = MSP_REC_STATUS_SUCCESS;    //识别状态
  int errcode = MSP_SUCCESS;

  FILE *f_pcm = NULL;
  char *p_pcm = NULL;
  long pcm_count = 0;
  long pcm_size = 0;
  long read_size = 0;

  if (NULL == audio_file)
    goto asr_exit;

  f_pcm = fopen(audio_file, "rb");
  if (NULL == f_pcm)
  {
    printf("\nopen [%s] failed!\n", audio_file);
    goto asr_exit;
  }

  fseek(f_pcm, 0, SEEK_END);
  pcm_size = ftell(f_pcm); //获取音频文件大小
  fseek(f_pcm, 0, SEEK_SET);

  p_pcm = (char *)malloc(pcm_size);
  if (NULL == p_pcm)
  {
    printf("\nout of memory!\n");
    goto asr_exit;
  }

  read_size = fread((void *)p_pcm, 1, pcm_size, f_pcm); //读取音频文件内容
  if (read_size != pcm_size)
  {
    printf("\nread [%s] failed!\n", audio_file);
    goto asr_exit;
  }

  printf("\n开始语音识别 ...\n");
  session_id = QISRSessionBegin(grammar_id, params, &errcode);
  if (MSP_SUCCESS != errcode)
  {
    printf("\nQISRSessionBegin failed, error code:%d\n", errcode);
    goto asr_exit;
  }

  while (1)
  {
    unsigned int len = 10 * FRAME_LEN; // 每次写入200ms音频(16k，16bit)：1帧音频20ms，10帧=200ms。16k采样率的16位音频，一帧的大小为640Byte
    int ret = 0;

    if (pcm_size < 2 * len)
      len = pcm_size;
    if (len <= 0)
      break;

    aud_stat = MSP_AUDIO_SAMPLE_CONTINUE;
    if (0 == pcm_count)
      aud_stat = MSP_AUDIO_SAMPLE_FIRST;

    printf(">");
    ret = QISRAudioWrite(session_id, (const void *)&p_pcm[pcm_count], len, aud_stat, &ep_stat, &rec_stat);
    if (MSP_SUCCESS != ret)
    {
      printf("\nQISRAudioWrite failed, error code:%d\n", ret);
      goto asr_exit;
    }

    pcm_count += (long)len;
    pcm_size -= (long)len;

    if (MSP_EP_AFTER_SPEECH == ep_stat)
      break;
    usleep(200 * 1000); //模拟人说话时间间隙，10帧的音频长度为200ms
  }
  errcode = QISRAudioWrite(session_id, NULL, 0, MSP_AUDIO_SAMPLE_LAST, &ep_stat, &rec_stat);
  if (MSP_SUCCESS != errcode)
  {
    printf("\nQISRAudioWrite failed, error code:%d\n", errcode);
    goto asr_exit;
  }

  while (MSP_REC_STATUS_COMPLETE != rec_stat)
  {
    const char *rslt = QISRGetResult(session_id, &rec_stat, 0, &errcode);
    if (MSP_SUCCESS != errcode)
    {
      printf("\nQISRGetResult failed, error code: %d\n", errcode);
      goto asr_exit;
    }
    if (NULL != rslt)
    {
      unsigned int rslt_len = strlen(rslt);
      total_len += rslt_len;
      if (total_len >= BUFFER_SIZE)
      {
        printf("\nno enough buffer for rec_result !\n");
        goto asr_exit;
      }
      strncat(rec_result, rslt, rslt_len);
    }
    usleep(150 * 1000); //防止频繁占用CPU
  }
  printf("\n语音识别结束\n");
  printf("=============================================================\n");
  printf("%s", rec_result);
  printf("=============================================================\n");

asr_exit:
  if (NULL != f_pcm)
  {
    fclose(f_pcm);
    f_pcm = NULL;
  }
  if (NULL != p_pcm)
  {
    free(p_pcm);
    p_pcm = NULL;
  }

  QISRSessionEnd(session_id, hints);
}

void Iat(const Nan::FunctionCallbackInfo<v8::Value> &info)
{
  // printf("%s", info[0] -> ToString());
  if (info.Length() != 6)
  {
    Nan::ThrowTypeError("Wrong number of arguments");
  }
  String::Utf8Value username(info[0]->ToString());
  String::Utf8Value password(info[1]->ToString());
  String::Utf8Value login_params(info[2]->ToString());
  String::Utf8Value session_params(info[3]->ToString());
  String::Utf8Value audio_file(info[4]->ToString());

  // printlog
  printf("(cc)>>>> username [%s].\n", (const char*)(*username));
  printf("(cc)>>>> password [%s].\n", (const char*)(*password));
  printf("(cc)>>>> login_params [%s].\n", (const char*)(*login_params));
  printf("(cc)>>>> session_params [%s].\n", (const char*)(*session_params));
  printf("(cc)>>>> audio_file [%s].\n", (const char*)(*audio_file));

  int ret = MSP_SUCCESS;
  // const char *login_params = "appid = 5864ae2d, work_dir = ."; //登录参数,appid与msc库绑定,请勿随意改动
  char *grammar_id = NULL;

  /* 用户登录 */
  ret = MSPLogin(NULL, NULL, (const char*)(*login_params)); //第一个参数是用户名，第二个参数是密码，均传NULL即可，第三个参数是登录参数
  if (MSP_SUCCESS != ret)
  {
    printf("MSPLogin failed, error code: %d.\n", ret);
  }
  else
  {
    printf("Logined.");
  }
  /*
	* sub:             请求业务类型
	* result_type:     识别结果格式
	* result_encoding: 结果编码格式
	*
	* 详细参数说明请参阅《讯飞语音云MSC--API文档》
	*/
  // const char *session_begin_params_asr = "sub = iat, result_type = plain, result_encoding = utf8";
  // const char *session_begin_params_iat = "sub = iat, domain = iat, language = zh_cn, accent = mandarin, sample_rate = 16000, result_type = plain, result_encoding = utf8";

  // run_asr("wav/iflytek01.wav", session_begin_params, grammar_id); //iflytek01对应的音频内容：“18012345678”
  run_iat((const char*)(*audio_file), (const char*)(*session_params));
  // run_iat("wav/iflytek01.wav", session_begin_params_iat);

  Isolate *isolate = info.GetIsolate();
  // callback fn
  Local<Function> cb = Local<Function>::Cast(info[5]);
  const unsigned argc = 1;
  Local<Value> argv[argc] = {String::NewFromUtf8(isolate, "hello world")};
  cb->Call(Null(isolate), argc, argv);
}

void RunCallback(const FunctionCallbackInfo<Value> &args)
{
  int ret = MSP_SUCCESS;
  const char *login_params = "appid = 5864ae2d, work_dir = ."; //登录参数,appid与msc库绑定,请勿随意改动
  char *grammar_id = NULL;

  /* 用户登录 */
  ret = MSPLogin(NULL, NULL, login_params); //第一个参数是用户名，第二个参数是密码，均传NULL即可，第三个参数是登录参数
  if (MSP_SUCCESS != ret)
  {
    printf("MSPLogin failed, error code: %d.\n", ret);
  }
  else
  {
    printf("Logined.");
  }
  /*
	* sub:             请求业务类型
	* result_type:     识别结果格式
	* result_encoding: 结果编码格式
	*
	* 详细参数说明请参阅《讯飞语音云MSC--API文档》
	*/
  const char *session_begin_params_asr = "sub = iat, result_type = plain, result_encoding = utf8";
  const char *session_begin_params_iat = "sub = iat, domain = iat, language = zh_cn, accent = mandarin, sample_rate = 16000, result_type = plain, result_encoding = utf8";

  // run_asr("wav/iflytek01.wav", session_begin_params, grammar_id); //iflytek01对应的音频内容：“18012345678”
  run_iat("wav/iflytek01.wav", session_begin_params_iat);

  Isolate *isolate = args.GetIsolate();
  Local<Function> cb = Local<Function>::Cast(args[0]);
  const unsigned argc = 1;
  Local<Value> argv[argc] = {String::NewFromUtf8(isolate, "hello world")};
  cb->Call(Null(isolate), argc, argv);
}

void Init(Local<Object> exports, Local<Object> module)
{
  exports->Set(Nan::New("iat").ToLocalChecked(),
               Nan::New<v8::FunctionTemplate>(Iat)->GetFunction());
}

NODE_MODULE(xfyun, Init)
} // namespace demo
