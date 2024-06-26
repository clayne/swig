
import director_binary_string.*;

public class director_binary_string_runme {

  static {
    try {
      System.loadLibrary("director_binary_string");
    } catch (UnsatisfiedLinkError e) {
      System.err.println("Native code library failed to load. See the chapter on Dynamic Linking Problems in the SWIG Java documentation for help.\n" + e);
      System.exit(1);
    }
  }

  public static void main(String argv[]) {
    Caller caller = new Caller();
    Callback callback = new DirectorBinaryStringCallback();
    caller.setCallback(callback);
    int sum = caller.call();
    int sumData = caller.callWriteData();
    caller.delCallback();

    if (sum != 9*2*8 + 13*3*5)
      throw new RuntimeException("Unexpected sum: " + sum);

    if (sumData != 9*2*8)
      throw new RuntimeException("Unexpected sumData: " + sumData);

    new Callback().run(null, null);
    callback = new DirectorBinaryStringCallback();
    caller.setCallback(callback);
    caller.call_null();
  }
}

class DirectorBinaryStringCallback extends Callback {
  public DirectorBinaryStringCallback() {
    super();
  }

  @Override
  public int run(String dataBufferAA, String dataBufferBB)
  {
    int ret = 0;
    if (dataBufferAA != null) {
      for (int i = 0; i < dataBufferAA.length(); i++)
        ret += (int)dataBufferAA.charAt(i) * 2;
    }

    if (dataBufferBB != null) {
      for (int i = 0; i < dataBufferBB.length(); i++) {
        ret += (int)dataBufferBB.charAt(i) * 3;
      }
    }
    return ret;
  }

  @Override
  public int writeData(String dataBufferAA)
  {
    int ret = 0;
    if (dataBufferAA != null) {
      for (int i = 0; i < dataBufferAA.length(); i++)
        ret += (int)dataBufferAA.charAt(i) * 2;
    }
    return ret;
  }
}

