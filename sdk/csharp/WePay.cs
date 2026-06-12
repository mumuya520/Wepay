using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace WePay
{
    /// <summary>
    /// WePay C# SDK
    /// 支持 MD5 和 RSA 签名的支付网关 SDK
    /// </summary>
    public class WePayClient
    {
        private string _mchId;
        private string _mchKey;
        private string _baseUrl;
        private string _signType;
        private RSA _privateKey;
        private HttpClient _httpClient;

        /// <summary>
        /// 初始化 SDK
        /// </summary>
        public WePayClient(string mchId, string mchKey, string baseUrl = "http://127.0.0.1:8088",
            string signType = "MD5", string privateKeyPem = "")
        {
            _mchId = mchId;
            _mchKey = mchKey;
            _baseUrl = baseUrl.TrimEnd('/');
            _signType = signType.ToUpper();
            _httpClient = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };

            if (_signType == "RSA" && !string.IsNullOrEmpty(privateKeyPem))
            {
                _privateKey = LoadPrivateKey(privateKeyPem);
            }
        }

        /// <summary>
        /// 加载 RSA 私钥
        /// </summary>
        private RSA LoadPrivateKey(string privateKeyPem)
        {
            var privateKeyContent = privateKeyPem
                .Replace("-----BEGIN PRIVATE KEY-----", "")
                .Replace("-----END PRIVATE KEY-----", "")
                .Replace("\r", "")
                .Replace("\n", "");

            var privateKeyBytes = Convert.FromBase64String(privateKeyContent);
            var rsa = RSA.Create();
            rsa.ImportPkcs8PrivateKey(privateKeyBytes, out _);
            return rsa;
        }

        /// <summary>
        /// 生成签名
        /// </summary>
        public string Sign(Dictionary<string, string> parameters)
        {
            if (_signType == "RSA")
            {
                return SignRsa(parameters);
            }
            else
            {
                return SignMd5(parameters);
            }
        }

        /// <summary>
        /// MD5 签名
        /// </summary>
        private string SignMd5(Dictionary<string, string> parameters)
        {
            var sortedParams = parameters
                .Where(p => p.Key != "sign" && p.Key != "sign_type" && !string.IsNullOrEmpty(p.Value))
                .OrderBy(p => p.Key)
                .ToList();

            var signStr = string.Join("&", sortedParams.Select(p => $"{p.Key}={p.Value}"));
            signStr += _mchKey;

            using (var md5 = MD5.Create())
            {
                var hash = md5.ComputeHash(Encoding.UTF8.GetBytes(signStr));
                return BitConverter.ToString(hash).Replace("-", "").ToLower();
            }
        }

        /// <summary>
        /// RSA-SHA256 签名
        /// </summary>
        private string SignRsa(Dictionary<string, string> parameters)
        {
            if (_privateKey == null)
            {
                throw new Exception("RSA 签名需要提供私钥");
            }

            var sortedParams = parameters
                .Where(p => p.Key != "sign" && p.Key != "sign_type" && !string.IsNullOrEmpty(p.Value))
                .OrderBy(p => p.Key)
                .ToList();

            var signStr = string.Join("&", sortedParams.Select(p => $"{p.Key}={p.Value}"));

            var signature = _privateKey.SignData(
                Encoding.UTF8.GetBytes(signStr),
                HashAlgorithmName.SHA256,
                RSASignaturePadding.Pkcs1);

            return Convert.ToBase64String(signature);
        }

        /// <summary>
        /// 统一下单
        /// </summary>
        public async Task<Dictionary<string, object>> CreateOrderAsync(Dictionary<string, string> parameters)
        {
            parameters["mch_id"] = _mchId;
            parameters["sign_type"] = _signType;
            parameters["sign"] = Sign(parameters);

            return await PostAsync("/gateway/create", parameters);
        }

        /// <summary>
        /// 查询订单
        /// </summary>
        public async Task<Dictionary<string, object>> QueryOrderAsync(string outTradeNo)
        {
            var parameters = new Dictionary<string, string>
            {
                { "mch_id", _mchId },
                { "out_trade_no", outTradeNo },
                { "sign_type", _signType }
            };
            parameters["sign"] = Sign(parameters);

            return await PostAsync("/gateway/query", parameters);
        }

        /// <summary>
        /// 申请退款
        /// </summary>
        public async Task<Dictionary<string, object>> RefundAsync(string tradeNo, string refundAmount)
        {
            var parameters = new Dictionary<string, string>
            {
                { "mch_id", _mchId },
                { "trade_no", tradeNo },
                { "refund_amount", refundAmount },
                { "sign_type", _signType }
            };
            parameters["sign"] = Sign(parameters);

            return await PostAsync("/gateway/refund", parameters);
        }

        /// <summary>
        /// 验证异步通知签名
        /// </summary>
        public bool VerifyNotify(Dictionary<string, string> parameters, string sign)
        {
            var calcSign = Sign(parameters);
            return calcSign == sign;
        }

        /// <summary>
        /// POST 请求
        /// </summary>
        private async Task<Dictionary<string, object>> PostAsync(string path, Dictionary<string, string> parameters)
        {
            var url = _baseUrl + path;
            var json = JsonConvert.SerializeObject(parameters);
            var content = new StringContent(json, Encoding.UTF8, "application/json");

            try
            {
                var response = await _httpClient.PostAsync(url, content);
                if (!response.IsSuccessStatusCode)
                {
                    throw new Exception($"HTTP 错误: {response.StatusCode}");
                }

                var responseContent = await response.Content.ReadAsStringAsync();
                return JsonConvert.DeserializeObject<Dictionary<string, object>>(responseContent);
            }
            catch (Exception ex)
            {
                throw new Exception($"请求失败: {ex.Message}");
            }
        }
    }

    /// <summary>
    /// 使用示例
    /// </summary>
    class Program
    {
        static async Task Main(string[] args)
        {
            var wepay = new WePayClient(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5"
            );

            var orderParams = new Dictionary<string, string>
            {
                { "out_trade_no", "order_csharp_" + DateTimeOffset.Now.ToUnixTimeMilliseconds() },
                { "pay_type", "wxpay" },
                { "amount", "0.01" },
                { "subject", "C# SDK 测试订单" }
            };

            try
            {
                var result = await wepay.CreateOrderAsync(orderParams);
                Console.WriteLine("下单结果:");
                Console.WriteLine(JsonConvert.SerializeObject(result, Formatting.Indented));
            }
            catch (Exception ex)
            {
                Console.WriteLine($"错误: {ex.Message}");
            }
        }
    }
}
