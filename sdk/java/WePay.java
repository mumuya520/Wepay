import java.io.*;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.*;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.*;
import javax.crypto.Cipher;
import com.google.gson.Gson;
import com.google.gson.JsonObject;
import org.apache.commons.codec.binary.Base64;
import org.apache.commons.codec.digest.DigestUtils;

/**
 * WePay Java SDK
 * 支持 MD5 和 RSA 签名的支付网关 SDK
 */
public class WePay {
    private String mchId;
    private String mchKey;
    private String baseUrl;
    private String signType;
    private String privateKeyPem;
    private Gson gson = new Gson();

    /**
     * 初始化 SDK
     */
    public WePay(String mchId, String mchKey, String baseUrl, String signType, String privateKeyPem) {
        this.mchId = mchId;
        this.mchKey = mchKey;
        this.baseUrl = baseUrl.replaceAll("/$", "");
        this.signType = signType.toUpperCase();
        this.privateKeyPem = privateKeyPem;
    }

    /**
     * 生成签名
     */
    public String sign(Map<String, String> params) throws Exception {
        if ("RSA".equals(signType)) {
            return signRsa(params);
        } else {
            return signMd5(params);
        }
    }

    /**
     * MD5 签名
     */
    private String signMd5(Map<String, String> params) {
        List<String> keys = new ArrayList<>(params.keySet());
        Collections.sort(keys);

        StringBuilder sb = new StringBuilder();
        for (String key : keys) {
            String value = params.get(key);
            if (!"sign".equals(key) && !"sign_type".equals(key) && value != null && !value.isEmpty()) {
                sb.append(key).append("=").append(value).append("&");
            }
        }
        sb.deleteCharAt(sb.length() - 1);
        sb.append(mchKey);

        return DigestUtils.md5Hex(sb.toString());
    }

    /**
     * RSA-SHA256 签名
     */
    private String signRsa(Map<String, String> params) throws Exception {
        if (privateKeyPem == null || privateKeyPem.isEmpty()) {
            throw new Exception("RSA 签名需要提供私钥");
        }

        List<String> keys = new ArrayList<>(params.keySet());
        Collections.sort(keys);

        StringBuilder sb = new StringBuilder();
        for (String key : keys) {
            String value = params.get(key);
            if (!"sign".equals(key) && !"sign_type".equals(key) && value != null && !value.isEmpty()) {
                sb.append(key).append("=").append(value).append("&");
            }
        }
        sb.deleteCharAt(sb.length() - 1);

        PrivateKey privateKey = loadPrivateKey(privateKeyPem);
        Signature signature = Signature.getInstance("SHA256withRSA");
        signature.initSign(privateKey);
        signature.update(sb.toString().getBytes(StandardCharsets.UTF_8));

        byte[] signBytes = signature.sign();
        return Base64.encodeBase64String(signBytes);
    }

    /**
     * 加载 RSA 私钥
     */
    private PrivateKey loadPrivateKey(String privateKeyPem) throws Exception {
        String privateKeyContent = privateKeyPem
            .replace("-----BEGIN PRIVATE KEY-----", "")
            .replace("-----END PRIVATE KEY-----", "")
            .replaceAll("\\s", "");

        byte[] decodedKey = Base64.decodeBase64(privateKeyContent);
        PKCS8EncodedKeySpec keySpec = new PKCS8EncodedKeySpec(decodedKey);
        KeyFactory keyFactory = KeyFactory.getInstance("RSA");
        return keyFactory.generatePrivate(keySpec);
    }

    /**
     * 统一下单
     */
    public JsonObject createOrder(Map<String, String> params) throws Exception {
        params.put("mch_id", mchId);
        params.put("sign_type", signType);
        params.put("sign", sign(params));

        return post("/gateway/create", params);
    }

    /**
     * 查询订单
     */
    public JsonObject queryOrder(String outTradeNo) throws Exception {
        Map<String, String> params = new HashMap<>();
        params.put("mch_id", mchId);
        params.put("out_trade_no", outTradeNo);
        params.put("sign_type", signType);
        params.put("sign", sign(params));

        return post("/gateway/query", params);
    }

    /**
     * 申请退款
     */
    public JsonObject refund(String tradeNo, String refundAmount) throws Exception {
        Map<String, String> params = new HashMap<>();
        params.put("mch_id", mchId);
        params.put("trade_no", tradeNo);
        params.put("refund_amount", refundAmount);
        params.put("sign_type", signType);
        params.put("sign", sign(params));

        return post("/gateway/refund", params);
    }

    /**
     * 验证异步通知签名
     */
    public boolean verifyNotify(Map<String, String> params, String sign) throws Exception {
        String calcSign = this.sign(params);
        return calcSign.equals(sign);
    }

    /**
     * POST 请求
     */
    private JsonObject post(String path, Map<String, String> params) throws Exception {
        URL url = new URL(baseUrl + path);
        HttpURLConnection conn = (HttpURLConnection) url.openConnection();
        conn.setRequestMethod("POST");
        conn.setRequestProperty("Content-Type", "application/json");
        conn.setDoOutput(true);
        conn.setConnectTimeout(10000);

        String jsonBody = gson.toJson(params);
        try (OutputStream os = conn.getOutputStream()) {
            os.write(jsonBody.getBytes(StandardCharsets.UTF_8));
        }

        int responseCode = conn.getResponseCode();
        if (responseCode != 200) {
            throw new Exception("HTTP 错误: " + responseCode);
        }

        StringBuilder response = new StringBuilder();
        try (BufferedReader br = new BufferedReader(new InputStreamReader(conn.getInputStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = br.readLine()) != null) {
                response.append(line);
            }
        }

        return gson.fromJson(response.toString(), JsonObject.class);
    }

    /**
     * 示例代码
     */
    public static void main(String[] args) {
        try {
            WePay wepay = new WePay(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5",
                ""
            );

            Map<String, String> orderParams = new HashMap<>();
            orderParams.put("out_trade_no", "order_java_" + System.currentTimeMillis());
            orderParams.put("pay_type", "wxpay");
            orderParams.put("amount", "0.01");
            orderParams.put("subject", "Java SDK 测试订单");

            JsonObject result = wepay.createOrder(orderParams);
            System.out.println("下单结果:");
            System.out.println(gson.toJson(result));
        } catch (Exception e) {
            System.err.println("错误: " + e.getMessage());
            e.printStackTrace();
        }
    }
}
